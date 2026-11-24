/*
    EDIScope - DAB+/EDI diagnostic player
    Copyright (C) 2025-2026 Yoann Queret

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "radiodns.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/nameser.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <resolv.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "xml_tree.h"


static const int DNS_PACKET_BUF_SIZE = NS_PACKETSZ * 4;
static const int HTTP_TIMEOUT_S = 5;


std::string RadioDNSResolver::DeriveGCC(int ecc, uint32_t sid, bool sid_32bit) {
	// ETSI TS 103 270 Annex A.1
	int country_nibble;
	int ecc_value;

	if(sid_32bit) {
		// data service: Country Code = 3rd nibble, ECC = 1st+2nd nibbles - both from the SId itself
		country_nibble = (sid >> 20) & 0xF;
		ecc_value = (sid >> 24) & 0xFF;
	} else {
		// audio service: Country Code = 1st nibble of the (16-bit) SId; ECC comes from FIG 0/9
		country_nibble = (sid >> 12) & 0xF;
		ecc_value = ecc;
	}

	if(ecc_value < 0 || ecc_value > 0xFF)
		return "";

	char buf[4];
	snprintf(buf, sizeof(buf), "%x%02x", country_nibble, ecc_value);
	return buf;
}

namespace {

void DABHexComponents(int eid, uint32_t sid, bool sid_32bit, int scids, std::string& eid_hex, std::string& sid_hex, std::string& scids_hex) {
	char buf[16];

	snprintf(buf, sizeof(buf), sid_32bit ? "%08x" : "%04x", sid);
	sid_hex = buf;

	snprintf(buf, sizeof(buf), "%04x", eid);
	eid_hex = buf;

	snprintf(buf, sizeof(buf), "%x", scids);
	scids_hex = buf;
}

}	// anonymous namespace

std::string RadioDNSResolver::BuildDABFQDN(const std::string& gcc, int eid, uint32_t sid, bool sid_32bit, int scids, const std::string& uatype) {
	std::string eid_hex, sid_hex, scids_hex;
	DABHexComponents(eid, sid, sid_32bit, scids, eid_hex, sid_hex, scids_hex);

	std::string fqdn;
	if(!uatype.empty())
		fqdn += uatype + ".";
	fqdn += scids_hex + "." + sid_hex + "." + eid_hex + "." + gcc + ".dab.radiodns.org";
	return fqdn;
}

std::string RadioDNSResolver::BuildDABBearerURI(const std::string& gcc, int eid, uint32_t sid, bool sid_32bit, int scids, const std::string& uatype) {
	std::string eid_hex, sid_hex, scids_hex;
	DABHexComponents(eid, sid, sid_32bit, scids, eid_hex, sid_hex, scids_hex);

	std::string uri = "dab:" + gcc + "." + eid_hex + "." + sid_hex + "." + scids_hex;
	if(!uatype.empty())
		uri += "." + uatype;
	return uri;
}

bool RadioDNSResolver::QueryCNAME(const std::string& fqdn, std::string& target, std::string& error) {
	unsigned char answer[DNS_PACKET_BUF_SIZE];
	int len = res_query(fqdn.c_str(), ns_c_in, ns_t_cname, answer, sizeof(answer));
	if(len < 0) {
		error = (h_errno == HOST_NOT_FOUND) ? "not registered with RadioDNS (NXDOMAIN)" :
				(h_errno == NO_DATA) ? "no CNAME record (not registered with RadioDNS)" :
				std::string("DNS query failed (") + hstrerror(h_errno) + ")";
		return false;
	}

	ns_msg handle;
	if(ns_initparse(answer, len, &handle) < 0) {
		error = "malformed DNS response";
		return false;
	}

	for(int i = 0; i < ns_msg_count(handle, ns_s_an); i++) {
		ns_rr rr;
		if(ns_parserr(&handle, ns_s_an, i, &rr) < 0)
			continue;
		if(ns_rr_type(rr) != ns_t_cname)
			continue;

		char name_buf[NS_MAXDNAME];
		if(dn_expand(ns_msg_base(handle), ns_msg_end(handle), ns_rr_rdata(rr), name_buf, sizeof(name_buf)) < 0)
			continue;

		target = name_buf;
		return true;
	}

	error = "no CNAME record in DNS response";
	return false;
}

bool RadioDNSResolver::QuerySRV(const std::string& fqdn, std::string& target, int& port, std::string& error) {
	unsigned char answer[DNS_PACKET_BUF_SIZE];
	int len = res_query(fqdn.c_str(), ns_c_in, ns_t_srv, answer, sizeof(answer));
	if(len < 0) {
		error = (h_errno == HOST_NOT_FOUND || h_errno == NO_DATA) ?
				"Authoritative FQDN found, but no SPI/EPG (_radioepg._tcp) SRV record" :
				std::string("DNS query failed (") + hstrerror(h_errno) + ")";
		return false;
	}

	ns_msg handle;
	if(ns_initparse(answer, len, &handle) < 0) {
		error = "malformed DNS response";
		return false;
	}

	// lowest SRV priority value wins (RFC 2782); weight is ignored (we only ever use one target)
	int best_priority = -1;
	for(int i = 0; i < ns_msg_count(handle, ns_s_an); i++) {
		ns_rr rr;
		if(ns_parserr(&handle, ns_s_an, i, &rr) < 0)
			continue;
		if(ns_rr_type(rr) != ns_t_srv)
			continue;

		const unsigned char *rdata = ns_rr_rdata(rr);
		if(ns_rr_rdlen(rr) < 6)
			continue;

		int priority = ns_get16(rdata);
		int rr_port = ns_get16(rdata + 4);

		char name_buf[NS_MAXDNAME];
		if(dn_expand(ns_msg_base(handle), ns_msg_end(handle), rdata + 6, name_buf, sizeof(name_buf)) < 0)
			continue;

		if(best_priority == -1 || priority < best_priority) {
			best_priority = priority;
			target = name_buf;
			port = rr_port;
		}
	}

	if(best_priority == -1) {
		error = "no usable SRV record in DNS response";
		return false;
	}
	return true;
}

int RadioDNSResolver::ConnectTCP(const std::string& host, int port, std::string& error) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo *addr_res = nullptr;
	int gai_err = getaddrinfo(host.c_str(), port_str, &hints, &addr_res);
	if(gai_err != 0) {
		error = std::string("can't resolve host '") + host + "': " + gai_strerror(gai_err);
		return -1;
	}

	// non-blocking connect with a timeout, rather than risking a connect() call that hangs for
	// minutes on a filtered/unreachable host
	int fd = -1;
	for(struct addrinfo *rp = addr_res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(fd < 0)
			continue;

		int flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		int conn_result = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if(conn_result == 0)
			break;	// connected immediately (rare for TCP, but possible)

		if(errno == EINPROGRESS) {
			struct pollfd pfd = {fd, POLLOUT, 0};
			if(poll(&pfd, 1, HTTP_TIMEOUT_S * 1000) > 0) {
				int so_error = 0;
				socklen_t so_len = sizeof(so_error);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
				if(so_error == 0)
					break;	// connected
			}
		}

		close(fd);
		fd = -1;
	}
	freeaddrinfo(addr_res);

	if(fd < 0) {
		error = "connection failed (timeout or refused)";
		return -1;
	}

	// back to blocking for the rest - simpler read/write below, still timeout-guarded
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	struct timeval tv;
	tv.tv_sec = HTTP_TIMEOUT_S;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	return fd;
}

bool RadioDNSResolver::ParseHttpResponse(const std::string& response, int& status, std::string& content_type, std::string& body, std::string& error) {
	if(response.empty()) {
		error = "no response from server (timeout?)";
		return false;
	}

	size_t header_end = response.find("\r\n\r\n");
	if(header_end == std::string::npos) {
		error = "malformed HTTP response (no header/body separator)";
		return false;
	}

	std::string header_block = response.substr(0, header_end);
	std::string rest = response.substr(header_end + 4);

	size_t status_line_end = header_block.find("\r\n");
	std::string status_line = header_block.substr(0, status_line_end);

	// "HTTP/1.1 200 OK"
	size_t sp1 = status_line.find(' ');
	if(sp1 == std::string::npos) {
		error = "malformed HTTP status line";
		return false;
	}
	size_t sp2 = status_line.find(' ', sp1 + 1);
	status = atoi(status_line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1).c_str());

	long content_length = -1;
	bool chunked = false;
	size_t pos = (status_line_end == std::string::npos ? header_block.size() : status_line_end) + 2;
	while(pos < header_block.size()) {
		size_t line_end = header_block.find("\r\n", pos);
		if(line_end == std::string::npos)
			line_end = header_block.size();
		std::string line = header_block.substr(pos, line_end - pos);
		pos = line_end + 2;

		size_t colon = line.find(':');
		if(colon == std::string::npos)
			continue;
		std::string key = line.substr(0, colon);
		std::string value = line.substr(colon + 1);
		size_t v_start = value.find_first_not_of(' ');
		value = v_start == std::string::npos ? "" : value.substr(v_start);

		std::transform(key.begin(), key.end(), key.begin(), ::tolower);

		if(key == "content-type")
			content_type = value;
		else if(key == "content-length")
			content_length = atol(value.c_str());
		else if(key == "transfer-encoding" && value.find("chunked") != std::string::npos)
			chunked = true;
	}

	if(chunked) {
		// minimal chunked-transfer decoding (RFC 7230 clause 4.1): a sequence of
		// <hex-size>\r\n<data>\r\n, terminated by a zero-size chunk
		std::string decoded;
		size_t p = 0;
		while(p < rest.size()) {
			size_t line_end = rest.find("\r\n", p);
			if(line_end == std::string::npos)
				break;
			size_t chunk_size = strtoul(rest.substr(p, line_end - p).c_str(), nullptr, 16);
			p = line_end + 2;
			if(chunk_size == 0)
				break;
			if(p + chunk_size > rest.size())
				break;	// truncated response
			decoded.append(rest, p, chunk_size);
			p += chunk_size + 2;	// skip the chunk's own trailing CRLF
		}
		body = decoded;
	} else if(content_length >= 0) {
		body = rest.substr(0, std::min((size_t) content_length, rest.size()));
	} else {
		// no Content-Length/chunked - what we already read (until the server closed the
		// connection, since the request asked for Connection: close) is the whole body
		body = rest;
	}

	return true;
}

bool RadioDNSResolver::HttpGet(const std::string& host, int port, const std::string& path, int& status, std::string& content_type, std::string& body, std::string& error) {
	int fd = ConnectTCP(host, port, error);
	if(fd < 0)
		return false;

	std::string request = "GET " + path + " HTTP/1.1\r\n"
			"Host: " + host + "\r\n"
			"User-Agent: EDIScope\r\n"
			"Accept: */*\r\n"
			"Connection: close\r\n"
			"\r\n";

	if(send(fd, request.c_str(), request.size(), 0) != (ssize_t) request.size()) {
		close(fd);
		error = "failed to send HTTP request to '" + host + "'";
		return false;
	}

	std::string response;
	char buf[8192];
	ssize_t n;
	while((n = recv(fd, buf, sizeof(buf), 0)) > 0)
		response.append(buf, n);
	close(fd);

	return ParseHttpResponse(response, status, content_type, body, error);
}

bool RadioDNSResolver::HttpsGet(const std::string& host, int port, const std::string& path, int& status, std::string& content_type, std::string& body, std::string& error) {
	int fd = ConnectTCP(host, port, error);
	if(fd < 0)
		return false;

	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	if(!ctx) {
		close(fd);
		error = "failed to create TLS context";
		return false;
	}

	SSL *ssl = SSL_new(ctx);
	SSL_set_fd(ssl, fd);
	SSL_set_tlsext_host_name(ssl, host.c_str());	// SNI - many hosts/CDNs need it to pick the right certificate/vhost

	bool ok = true;
	std::string response;

	if(SSL_connect(ssl) != 1) {
		char err_buf[256];
		ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
		error = std::string("TLS handshake failed: ") + err_buf;
		ok = false;
	}

	if(ok) {
		std::string request = "GET " + path + " HTTP/1.1\r\n"
				"Host: " + host + "\r\n"
				"User-Agent: EDIScope\r\n"
				"Accept: */*\r\n"
				"Connection: close\r\n"
				"\r\n";

		if(SSL_write(ssl, request.c_str(), (int) request.size()) <= 0) {
			error = "failed to send HTTPS request to '" + host + "'";
			ok = false;
		}
	}

	if(ok) {
		char buf[8192];
		int n;
		while((n = SSL_read(ssl, buf, sizeof(buf))) > 0)
			response.append(buf, n);
	}

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	close(fd);

	if(!ok)
		return false;

	return ParseHttpResponse(response, status, content_type, body, error);
}

bool RadioDNSResolver::HttpGetUrl(const std::string& url, int& status, std::string& content_type, std::string& body, std::string& error) {
	static const std::string PREFIX_HTTP = "http://";
	static const std::string PREFIX_HTTPS = "https://";

	bool is_https;
	std::string rest;
	int default_port;
	if(url.compare(0, PREFIX_HTTPS.size(), PREFIX_HTTPS) == 0) {
		is_https = true;
		rest = url.substr(PREFIX_HTTPS.size());
		default_port = 443;
	} else if(url.compare(0, PREFIX_HTTP.size(), PREFIX_HTTP) == 0) {
		is_https = false;
		rest = url.substr(PREFIX_HTTP.size());
		default_port = 80;
	} else {
		error = "unsupported URL scheme";
		return false;
	}

	size_t slash = rest.find('/');
	std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
	std::string path = slash == std::string::npos ? "/" : rest.substr(slash);

	std::string host = host_port;
	int port = default_port;
	size_t colon = host_port.find(':');
	if(colon != std::string::npos) {
		host = host_port.substr(0, colon);
		port = atoi(host_port.substr(colon + 1).c_str());
	}

	if(host.empty() || port <= 0) {
		error = "malformed URL '" + url + "'";
		return false;
	}

	return is_https ? HttpsGet(host, port, path, status, content_type, body, error) :
			HttpGet(host, port, path, status, content_type, body, error);
}

namespace {

// a <serviceProvider> or <service> element shares the same child structure for the fields we care
// about (ETSI TS 102 818 clause 5.2.2.1/5.2.2.2) - only <service> additionally carries <bearer>s
void ExtractSIService(const XMLNode& node, RDNS_XML_SERVICE& out) {
	if(const XMLNode *n = node.FindChild("shortName")) out.short_name = n->text;
	if(const XMLNode *n = node.FindChild("mediumName")) out.medium_name = n->text;
	if(const XMLNode *n = node.FindChild("longName")) out.long_name = n->text;

	for(const XMLNode *md : node.FindChildren("mediaDescription")) {
		if(out.short_description.empty())
			if(const XMLNode *n = md->FindChild("shortDescription"))
				out.short_description = n->text;
		if(out.long_description.empty())
			if(const XMLNode *n = md->FindChild("longDescription"))
				out.long_description = n->text;

		if(const XMLNode *n = md->FindChild("multimedia")) {
			std::string url = n->Attr("url");
			if(!url.empty()) {
				RDNS_XML_LOGO logo;
				logo.url = url;
				logo.type = n->Attr("type");
				logo.width = atoi(n->Attr("width", "0").c_str());
				logo.height = atoi(n->Attr("height", "0").c_str());
				logo.mime = n->Attr("mimeValue");
				out.logos.push_back(logo);
			}
		}
	}

	for(const XMLNode *b : node.FindChildren("bearer")) {
		RDNS_XML_BEARER bearer;
		bearer.id = b->Attr("id");
		bearer.cost = b->Attr("cost");
		bearer.bitrate = b->Attr("bitrate");
		// seen in the wild as both "mimevalue" and "mimeValue" depending on the SI generator
		bearer.mime = b->Attr("mimevalue", b->Attr("mimeValue"));
		if(!bearer.id.empty())
			out.bearers.push_back(bearer);
	}
}

}	// anonymous namespace

bool RadioDNSResolver::ParseSIXml(const std::string& xml, const std::string& own_bearer_uri, RDNS_XML_SERVICE& out_provider, RDNS_XML_SERVICE& out_service, bool& out_service_matched) {
	out_service_matched = false;

	XMLNode root;
	if(!XMLParser::Parse(xml, root) || root.tag != "serviceInformation")
		return false;

	const XMLNode *services_node = root.FindChild("services");
	if(!services_node)
		return false;

	if(const XMLNode *provider_node = services_node->FindChild("serviceProvider"))
		ExtractSIService(*provider_node, out_provider);

	for(const XMLNode *service_node : services_node->FindChildren("service")) {
		RDNS_XML_SERVICE service;
		ExtractSIService(*service_node, service);

		bool is_own = false;
		for(const RDNS_XML_BEARER& bearer : service.bearers)
			if(strcasecmp(bearer.id.c_str(), own_bearer_uri.c_str()) == 0)
				is_own = true;

		if(is_own) {
			out_service = service;
			out_service_matched = true;
			break;
		}
	}

	return true;
}

bool RadioDNSResolver::FetchLogo(RDNS_XML_LOGO& logo) {
	int status = 0;
	std::string content_type, error, body;
	if(!HttpGetUrl(logo.url, status, content_type, body, error) || status != 200) {
		logo.data.clear();
		return false;
	}
	logo.data.assign(body.begin(), body.end());
	return true;
}

RDNS_RESULT RadioDNSResolver::Resolve(int ecc, int eid, uint32_t sid, bool sid_32bit, int scids) {
	RDNS_RESULT result;

	result.gcc = DeriveGCC(ecc, sid, sid_32bit);
	if(result.gcc.empty()) {
		result.cname.attempted = true;
		result.cname.detail = "can't derive the GCC (ETSI TS 103 270 Annex A.1): this service's ECC isn't known yet";
		return result;
	}

	result.fqdn = BuildDABFQDN(result.gcc, eid, sid, sid_32bit, scids);

	result.cname.attempted = true;
	result.cname.success = QueryCNAME(result.fqdn, result.authoritative_fqdn, result.cname.detail);
	if(result.cname.success)
		result.cname.detail = "Authoritative FQDN: " + result.authoritative_fqdn;
	if(!result.cname.success)
		return result;

	result.srv.attempted = true;
	result.srv.success = QuerySRV("_radioepg._tcp." + result.authoritative_fqdn, result.srv_target, result.srv_port, result.srv.detail);
	if(result.srv.success)
		result.srv.detail = "SPI server: " + result.srv_target + ":" + std::to_string(result.srv_port);
	if(!result.srv.success)
		return result;

	result.http.attempted = true;
	// path per RadioDNS's "HOWTO - Make an SI file accessible to RadioDNS Hybrid Radio Devices"
	bool http_exchange_ok = HttpGet(result.srv_target, result.srv_port, "/radiodns/spi/3.1/SI.xml", result.http_status, result.http_content_type, result.si_xml, result.http.detail);
	if(http_exchange_ok && result.http_status == 200) {
		result.http.success = true;
		result.http.detail = "HTTP 200 OK, " + std::to_string(result.si_xml.size()) + " bytes (" + result.http_content_type + ")";
	} else if(http_exchange_ok) {
		result.http.success = false;
		result.http.detail = "HTTP " + std::to_string(result.http_status) + " (expected 200)";
	}
	// else: result.http.detail was already set by HttpGet() to the connection/protocol error

	if(result.http.success) {
		std::string own_bearer_uri = BuildDABBearerURI(result.gcc, eid, sid, sid_32bit, scids);
		result.si_parsed = ParseSIXml(result.si_xml, own_bearer_uri, result.si_provider, result.si_service, result.si_service_matched);

		if(result.si_parsed) {
			for(RDNS_XML_LOGO& logo : result.si_provider.logos)
				FetchLogo(logo);
			for(RDNS_XML_LOGO& logo : result.si_service.logos)
				FetchLogo(logo);
		}
	}

	return result;
}
