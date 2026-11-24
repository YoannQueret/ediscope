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

#ifndef RADIODNS_H_
#define RADIODNS_H_

#include <cstdint>
#include <string>
#include <vector>


// --- RDNS_XML_LOGO -----------------------------------------------------------------
// one <multimedia> (logo) entry of a RadioDNS SI.xml service/serviceProvider (ETSI TS 102 818
// clause 5.8/Annex D) - unlike the OTA MOT-carried SPI variant, its url is a plain HTTP(S) URL,
// fetched independently of the broadcast carousel
struct RDNS_XML_LOGO {
	std::string url;
	std::string type;	// "logo_unrestricted"/"logo_colour_square"/"logo_colour_rectangle"
	int width;
	int height;
	std::string mime;
	std::vector<uint8_t> data;	// fetched image bytes - empty if not fetched yet, or the fetch failed

	RDNS_XML_LOGO() : width(0), height(0) {}
};

// --- RDNS_XML_BEARER -----------------------------------------------------------------
// one <bearer> entry (ETSI TS 102 818 clause 5.5) - a way to actually receive this service,
// DAB ("dab:...", ETSI TS 103 270 clause 5.1.2.4 bearerURI) or an IP stream (a plain URI)
struct RDNS_XML_BEARER {
	std::string id;
	std::string cost;
	std::string bitrate;
	std::string mime;
};

// --- RDNS_XML_SERVICE -----------------------------------------------------------------
// a <serviceProvider> or <service> entry of a fetched SI.xml, whichever fields it had
struct RDNS_XML_SERVICE {
	std::string short_name;
	std::string medium_name;
	std::string long_name;
	std::string short_description;
	std::string long_description;
	std::vector<RDNS_XML_LOGO> logos;
	std::vector<RDNS_XML_BEARER> bearers;	// always empty for a serviceProvider entry

	bool IsEmpty() const {return short_name.empty() && medium_name.empty() && long_name.empty();}
};


// --- RDNS_STAGE -----------------------------------------------------------------
// one step of the RadioDNS resolution chain - lets the caller show exactly where (if anywhere)
// the chain breaks down for a given service, instead of just a final yes/no
struct RDNS_STAGE {
	bool attempted;
	bool success;
	std::string detail;	// human-readable outcome or error - always set once attempted

	RDNS_STAGE() : attempted(false), success(false) {}
};

// --- RDNS_RESULT -----------------------------------------------------------------
// the full trace of one service's RadioDNS resolution attempt
struct RDNS_RESULT {
	std::string gcc;	// empty if it couldn't even be derived (see cname.detail in that case)
	std::string fqdn;	// the RadioDNS FQDN built from the broadcast parameters (ETSI TS 103 270 clause 5.1.2.2)

	RDNS_STAGE cname;	// RadioDNS FQDN -> Authoritative FQDN (ETSI TS 103 270 clause 5.2)
	std::string authoritative_fqdn;

	RDNS_STAGE srv;		// _radioepg._tcp.<Authoritative FQDN> DNS SRV lookup (RadioDNS SPI HOWTO)
	std::string srv_target;
	int srv_port;

	RDNS_STAGE http;	// HTTP GET http://<srv_target>:<srv_port>/radiodns/spi/3.1/SI.xml
	int http_status;
	std::string http_content_type;
	std::string si_xml;

	// the fetched si_xml, decoded (ETSI TS 102 818) - si_parsed is false if si_xml wasn't even
	// well-formed XML (si_provider/si_service/si_service_matched are then meaningless). The SI.xml
	// lists every service of this provider, not just this one, so si_service is the one entry (if
	// any) whose own <bearer> matches this service's own DAB bearerURI (clause 5.1.2.4)
	bool si_parsed;
	RDNS_XML_SERVICE si_provider;
	RDNS_XML_SERVICE si_service;
	bool si_service_matched;

	RDNS_RESULT() : srv_port(0), http_status(0), si_parsed(false), si_service_matched(false) {}

	// true once the chain made it all the way through to a fetched SI.xml
	bool FullyResolved() const {return http.attempted && http.success;}
};


// --- RadioDNSResolver -----------------------------------------------------------------
// Resolves a DAB service's RadioDNS/Hybrid Radio chain: builds the RadioDNS FQDN from its
// broadcast parameters (ETSI TS 103 270 clause 5.1.2.2), resolves it to an Authoritative FQDN
// (DNS CNAME, clause 5.2), looks up that FQDN's SPI/EPG application server (DNS SRV
// _radioepg._tcp, per RadioDNS's own "HOWTO - Add SRV records to your Authoritative FQDN") and
// fetches its Service Information document over HTTP (/radiodns/spi/3.1/SI.xml, per RadioDNS's
// own "HOWTO - Make an SI file accessible...") - exactly the chain a RadioDNS Hybrid Radio
// receiver itself performs. Entirely blocking/synchronous (DNS + HTTP I/O): always run on a
// background thread, never on the GTK main thread.
class RadioDNSResolver {
public:
	// runs the whole chain for one DAB service and returns a result with every stage it managed
	// to attempt. ecc/eid/sid/scids are the broadcast parameters as decoded from the FIC
	// (FIC_ENSEMBLE/LISTED_SERVICE); sid_32bit is true for a data service's 32-bit SId, false for
	// an audio service's 16-bit one (see DeriveGCC())
	static RDNS_RESULT Resolve(int ecc, int eid, uint32_t sid, bool sid_32bit, int scids);

	// ETSI TS 103 270 Annex A.1: GCC = (1-char Country Code) + (2-char ECC), lowercase hex. For an
	// audio service (16-bit SId), the Country Code is the SId's own first nibble and ecc comes
	// from FIG 0/9 (ecc_none/-1 if not yet known, in which case this returns ""); for a data
	// service (32-bit SId), both the Country Code (3rd nibble) and the ECC (1st+2nd nibbles) are
	// embedded in the SId itself, and the passed-in ecc is ignored.
	static std::string DeriveGCC(int ecc, uint32_t sid, bool sid_32bit);

	// ETSI TS 103 270 clause 5.1.2.2: "[<uatype>.]<scids>.<sid>.<eid>.<gcc>.dab.radiodns.org"
	static std::string BuildDABFQDN(const std::string& gcc, int eid, uint32_t sid, bool sid_32bit, int scids, const std::string& uatype = "");

	// ETSI TS 103 270 clause 5.1.2.4: "dab:<gcc>.<eid>.<sid>.<scids>[.<uatype>]" - how this exact
	// service is identified inside a RadioDNS SI.xml's own <bearer id="..."> (see ParseSIXml())
	static std::string BuildDABBearerURI(const std::string& gcc, int eid, uint32_t sid, bool sid_32bit, int scids, const std::string& uatype = "");

	// decodes a RadioDNS SI.xml document (ETSI TS 102 818) - returns false if it isn't even
	// well-formed XML/a <serviceInformation> document; a best-effort partial result is still
	// filled in otherwise, matching this format's own "ignore anything you don't understand"
	// philosophy. own_bearer_uri (see BuildDABBearerURI()) picks which <service> entry, if any, is
	// this one - out_service_matched is false if none of them carried that bearer
	static bool ParseSIXml(const std::string& xml, const std::string& own_bearer_uri, RDNS_XML_SERVICE& out_provider, RDNS_XML_SERVICE& out_service, bool& out_service_matched);

	// fetches one logo's pixel data over HTTP or HTTPS, whichever its own url uses - many
	// real-world providers serve logos (often via a CDN) over HTTPS even though an SI.xml is
	// never required to use it
	static bool FetchLogo(RDNS_XML_LOGO& logo);

private:
	static bool QueryCNAME(const std::string& fqdn, std::string& target, std::string& error);
	static bool QuerySRV(const std::string& fqdn, std::string& target, int& port, std::string& error);

	// plain TCP connect with a timeout, shared by HttpGet() and HttpsGet() - returns the
	// connected socket fd, or -1 (with error set) on failure
	static int ConnectTCP(const std::string& host, int port, std::string& error);

	// shared response parsing (status line, headers, chunked/Content-Length body) for whatever
	// HttpGet()/HttpsGet() already read off the wire
	static bool ParseHttpResponse(const std::string& response, int& status, std::string& content_type, std::string& body, std::string& error);

	static bool HttpGet(const std::string& host, int port, const std::string& path, int& status, std::string& content_type, std::string& body, std::string& error);

	// same as HttpGet(), over TLS (SNI is set, but the server's certificate is NOT validated -
	// this is a diagnostic tool fetching publicly-served SI.xml/logo assets, not a security
	// boundary, and shipping a CA trust store/verification policy is out of scope)
	static bool HttpsGet(const std::string& host, int port, const std::string& path, int& status, std::string& content_type, std::string& body, std::string& error);

	static bool HttpGetUrl(const std::string& url, int& status, std::string& content_type, std::string& body, std::string& error);
};

#endif /* RADIODNS_H_ */
