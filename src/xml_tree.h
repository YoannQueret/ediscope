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

#ifndef XML_TREE_H_
#define XML_TREE_H_

#include <map>
#include <string>
#include <vector>


// --- XMLNode -----------------------------------------------------------------
// one element of a parsed XML document - just enough of a DOM-ish tree (tag, attributes, direct
// text, children) to read a document like a RadioDNS SI.xml; not a validating/namespace-aware
// parser, and not meant for arbitrary/adversarial XML
struct XMLNode {
	std::string tag;
	std::map<std::string,std::string> attrs;
	std::string text;	// direct text content of this element (trimmed, entities decoded)
	std::vector<XMLNode> children;

	// first direct child with this tag, or nullptr
	const XMLNode* FindChild(const std::string& child_tag) const;
	// every direct child with this tag, in document order
	std::vector<const XMLNode*> FindChildren(const std::string& child_tag) const;
	// attribute value, or fallback if absent
	std::string Attr(const std::string& name, const std::string& fallback = "") const;
};


// --- XMLParser -----------------------------------------------------------------
class XMLParser {
public:
	// parses a full XML document into its root element - returns false if malformed (no partial
	// best-effort recovery: a RadioDNS SI.xml is either well-formed, or shown as raw text as-is)
	static bool Parse(const std::string& xml, XMLNode& root);
};

#endif /* XML_TREE_H_ */
