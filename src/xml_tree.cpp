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

#include "xml_tree.h"

#include <cctype>
#include <cstdlib>
#include <cstring>


const XMLNode* XMLNode::FindChild(const std::string& child_tag) const {
	for(const XMLNode& child : children)
		if(child.tag == child_tag)
			return &child;
	return nullptr;
}

std::vector<const XMLNode*> XMLNode::FindChildren(const std::string& child_tag) const {
	std::vector<const XMLNode*> result;
	for(const XMLNode& child : children)
		if(child.tag == child_tag)
			result.push_back(&child);
	return result;
}

std::string XMLNode::Attr(const std::string& name, const std::string& fallback) const {
	std::map<std::string,std::string>::const_iterator it = attrs.find(name);
	return it != attrs.end() ? it->second : fallback;
}


// --- parsing helpers (internal) -----------------------------------------------------------------
namespace {

std::string DecodeEntities(const std::string& raw) {
	std::string out;
	out.reserve(raw.size());

	for(size_t i = 0; i < raw.size();) {
		if(raw[i] != '&') {
			out += raw[i++];
			continue;
		}

		size_t semi = raw.find(';', i + 1);
		if(semi == std::string::npos || semi - i > 10) {	// not a (plausible) entity - keep the '&' as-is
			out += raw[i++];
			continue;
		}

		std::string name = raw.substr(i + 1, semi - i - 1);
		if(name == "amp") out += '&';
		else if(name == "lt") out += '<';
		else if(name == "gt") out += '>';
		else if(name == "quot") out += '"';
		else if(name == "apos") out += '\'';
		else if(!name.empty() && name[0] == '#') {
			// numeric character reference, decimal (&#NNN;) or hex (&#xHHHH;) - encoded as UTF-8,
			// since that's what every string in this codebase is assumed to already be in
			long cp = (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ?
					strtol(name.c_str() + 2, nullptr, 16) : strtol(name.c_str() + 1, nullptr, 10);
			if(cp < 0x80) {
				out += (char) cp;
			} else if(cp < 0x800) {
				out += (char) (0xC0 | (cp >> 6));
				out += (char) (0x80 | (cp & 0x3F));
			} else if(cp < 0x10000) {
				out += (char) (0xE0 | (cp >> 12));
				out += (char) (0x80 | ((cp >> 6) & 0x3F));
				out += (char) (0x80 | (cp & 0x3F));
			} else {
				out += (char) (0xF0 | (cp >> 18));
				out += (char) (0x80 | ((cp >> 12) & 0x3F));
				out += (char) (0x80 | ((cp >> 6) & 0x3F));
				out += (char) (0x80 | (cp & 0x3F));
			}
		} else {
			// unknown entity - keep it verbatim rather than guessing/dropping it
			out += '&' + name + ';';
		}

		i = semi + 1;
	}

	return out;
}

std::string Trim(const std::string& s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if(start == std::string::npos)
		return "";
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

// minimal recursive-descent parser over the whole document string, tracking one cursor position
class Parser {
public:
	Parser(const std::string& xml) : s(xml), pos(0) {}

	bool ParseDocument(XMLNode& root) {
		SkipProlog();
		SkipMisc();
		if(pos >= s.size() || s[pos] != '<')
			return false;
		return ParseElement(root);
	}

private:
	const std::string& s;
	size_t pos;

	void SkipWhitespace() {
		while(pos < s.size() && std::isspace((unsigned char) s[pos]))
			pos++;
	}

	bool StartsWith(const char *literal) {
		size_t len = strlen(literal);
		return s.compare(pos, len, literal) == 0;
	}

	void SkipProlog() {
		SkipWhitespace();
		if(StartsWith("<?")) {
			size_t end = s.find("?>", pos);
			pos = (end == std::string::npos) ? s.size() : end + 2;
		}
	}

	// comments and a DOCTYPE, if any, between the prolog and the root element
	void SkipMisc() {
		for(;;) {
			SkipWhitespace();
			if(StartsWith("<!--")) {
				size_t end = s.find("-->", pos);
				pos = (end == std::string::npos) ? s.size() : end + 3;
			} else if(StartsWith("<!")) {
				size_t end = s.find('>', pos);
				pos = (end == std::string::npos) ? s.size() : end + 1;
			} else {
				break;
			}
		}
	}

	static bool IsNameChar(char c) {
		return std::isalnum((unsigned char) c) || c == '_' || c == '-' || c == '.' || c == ':';
	}

	std::string ParseName() {
		size_t start = pos;
		while(pos < s.size() && IsNameChar(s[pos]))
			pos++;
		return s.substr(start, pos - start);
	}

	bool ParseAttributes(XMLNode& node) {
		for(;;) {
			SkipWhitespace();
			if(pos >= s.size())
				return false;
			if(s[pos] == '/' || s[pos] == '>')
				return true;

			std::string name = ParseName();
			if(name.empty())
				return false;

			SkipWhitespace();
			if(pos >= s.size() || s[pos] != '=')
				return false;
			pos++;	// '='
			SkipWhitespace();
			if(pos >= s.size() || (s[pos] != '"' && s[pos] != '\''))
				return false;
			char quote = s[pos++];
			size_t value_start = pos;
			size_t value_end = s.find(quote, pos);
			if(value_end == std::string::npos)
				return false;
			node.attrs[name] = DecodeEntities(s.substr(value_start, value_end - value_start));
			pos = value_end + 1;
		}
	}

	// parses one element (the opening '<' is expected still unconsumed at the current position)
	bool ParseElement(XMLNode& node) {
		if(pos >= s.size() || s[pos] != '<')
			return false;
		pos++;	// '<'

		node.tag = ParseName();
		if(node.tag.empty())
			return false;

		if(!ParseAttributes(node))
			return false;

		SkipWhitespace();
		if(pos < s.size() && s[pos] == '/') {	// self-closing: "/>"
			pos++;
			if(pos >= s.size() || s[pos] != '>')
				return false;
			pos++;
			return true;
		}
		if(pos >= s.size() || s[pos] != '>')
			return false;
		pos++;	// '>'

		std::string text_accum;
		for(;;) {
			size_t next_tag = s.find('<', pos);
			if(next_tag == std::string::npos)
				return false;	// unterminated element

			text_accum += s.substr(pos, next_tag - pos);
			pos = next_tag;

			if(StartsWith("<!--")) {
				size_t end = s.find("-->", pos);
				pos = (end == std::string::npos) ? s.size() : end + 3;
				continue;
			}
			if(StartsWith("<![CDATA[")) {
				size_t end = s.find("]]>", pos + 9);
				if(end == std::string::npos)
					return false;
				text_accum += s.substr(pos + 9, end - (pos + 9));
				pos = end + 3;
				continue;
			}
			if(StartsWith("</")) {
				pos += 2;
				ParseName();	// the closing tag's own name - not re-validated against node.tag
				SkipWhitespace();
				if(pos >= s.size() || s[pos] != '>')
					return false;
				pos++;
				node.text = Trim(DecodeEntities(text_accum));
				return true;
			}

			// a nested element
			node.children.emplace_back();
			if(!ParseElement(node.children.back()))
				return false;
		}
	}
};

}	// anonymous namespace


bool XMLParser::Parse(const std::string& xml, XMLNode& root) {
	Parser parser(xml);
	return parser.ParseDocument(root);
}
