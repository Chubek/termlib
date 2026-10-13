/** @file Jiterati-DocIR.hpp
 *  @brief DocIR: DOM-based document IR with XtAs/markup serializers + CSSOM.
 *
 *  DocIR models documents as a typed DOM (Document > Element > Text) plus a
 *  CSS object model (StyleSheet > CssRule). Serializers emit:
 *
 *  - XtAs: canonical XML application syntax (`xt:as` namespace);
 *  - HTML5 and Markdown mirrors of the same DOM;
 *  - CSSOM text via @ref Stylesheet::to_css().
 *
 *  Document creation kernels (DocIR/Kernels.*) build Documents from live
 *  Jiterati metadata: modules, backends, passes, CLI references.
 *
 *  Header-only DOM/CSSOM core; kernels link from DocIR/Kernels.cpp.
 */
#ifndef JITERATI_DOCIR_HPP_INCLUDED
#define JITERATI_DOCIR_HPP_INCLUDED

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace jiterati::docir {

/** Escaping helpers shared by serializers. */
inline std::string escape_xml(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

/** DOM node kinds. */
enum class NodeKind { Element, Text, Comment };

/** A single DOM node. Elements own children; text/comment hold literals. */
class Node {
public:
    static std::shared_ptr<Node> element(std::string tag) {
        auto node = std::make_shared<Node>(NodeKind::Element);
        node->tag_ = std::move(tag);
        return node;
    }
    static std::shared_ptr<Node> text(std::string content) {
        auto node = std::make_shared<Node>(NodeKind::Text);
        node->content_ = std::move(content);
        return node;
    }
    static std::shared_ptr<Node> comment(std::string content) {
        auto node = std::make_shared<Node>(NodeKind::Comment);
        node->content_ = std::move(content);
        return node;
    }

    explicit Node(NodeKind kind) : kind_(kind) {}

    NodeKind kind() const { return kind_; }
    std::string const& tag() const { return tag_; }
    std::string const& content() const { return content_; }

    Node& attr(std::string key, std::string value) {
        attrs_[std::move(key)] = std::move(value);
        return *this;
    }
    Node& add(std::shared_ptr<Node> child) {
        children_.push_back(std::move(child));
        return *this;
    }
    Node& add_text(std::string content) {
        return add(text(std::move(content)));
    }

    std::map<std::string, std::string> const& attrs() const { return attrs_; }
    std::vector<std::shared_ptr<Node>> const& children() const { return children_; }

    /** Serialize this subtree as XtAs (canonical XML, xt:as namespace). */
    void to_xtas(std::ostream& out, int indent = 0) const {
        std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
        if (kind_ == NodeKind::Text) {
            out << pad << escape_xml(content_) << "\n";
            return;
        }
        if (kind_ == NodeKind::Comment) {
            out << pad << "<!--" << escape_xml(content_) << "-->\n";
            return;
        }
        out << pad << "<" << tag_;
        for (auto const& [key, value] : attrs_)
            out << " " << key << "=\"" << escape_xml(value) << "\"";
        if (children_.empty()) {
            out << "/>\n";
            return;
        }
        out << ">\n";
        for (auto const& child : children_) child->to_xtas(out, indent + 1);
        out << pad << "</" << tag_ << ">\n";
    }

    std::string to_xtas() const {
        std::ostringstream out;
        to_xtas(out, 0);
        return out.str();
    }

    /** Serialize as HTML5 (void elements respected). */
    void to_html(std::ostream& out) const {
        if (kind_ == NodeKind::Text) {
            out << escape_xml(content_);
            return;
        }
        if (kind_ == NodeKind::Comment) {
            out << "<!--" << escape_xml(content_) << "-->";
            return;
        }
        out << "<" << tag_;
        for (auto const& [key, value] : attrs_)
            out << " " << key << "=\"" << escape_xml(value) << "\"";
        static const char* voids[] = {"br", "hr", "img", "link", "meta", nullptr};
        for (const char** v = voids; *v; ++v)
            if (tag_ == *v) {
                out << ">";
                return;
            }
        out << ">";
        for (auto const& child : children_) child->to_html(out);
        out << "</" << tag_ << ">";
    }

    std::string to_html() const {
        std::ostringstream out;
        to_html(out);
        return out.str();
    }

private:
    NodeKind kind_;
    std::string tag_;
    std::string content_;
    std::map<std::string, std::string> attrs_;
    std::vector<std::shared_ptr<Node>> children_;
};

/** A complete document: root element plus title/language metadata. */
class Document {
public:
    Document() : root_(Node::element("article")) {}
    explicit Document(std::string root_tag) : root_(Node::element(std::move(root_tag))) {}

    Node& root() { return *root_; }
    Node const& root() const { return *root_; }

    Document& title(std::string value) {
        title_ = std::move(value);
        return *this;
    }
    Document& lang(std::string value) {
        lang_ = std::move(value);
        return *this;
    }
    Document& stylesheet(std::string href) {
        stylesheets_.push_back(std::move(href));
        return *this;
    }

    std::string const& title() const { return title_; }

    std::string to_xtas() const {
        std::ostringstream out;
        out << "<?xt version=\"1.0\" as=\"xt:as\"?>\n";
        out << "<document title=\"" << escape_xml(title_) << "\" lang=\""
            << escape_xml(lang_) << "\">\n";
        root_->to_xtas(out, 1);
        out << "</document>\n";
        return out.str();
    }

    std::string to_html() const {
        std::ostringstream out;
        out << "<!DOCTYPE html><html lang=\"" << escape_xml(lang_) << "\">"
            << "<head><meta charset=\"utf-8\"><title>" << escape_xml(title_)
            << "</title>";
        for (auto const& href : stylesheets_)
            out << "<link rel=\"stylesheet\" href=\"" << escape_xml(href) << "\">";
        out << "</head><body>";
        root_->to_html(out);
        out << "</body></html>";
        return out.str();
    }

private:
    std::shared_ptr<Node> root_;
    std::string title_;
    std::string lang_ = "en";
    std::vector<std::string> stylesheets_;
};

/** One CSS rule: selector + declaration block. */
struct CssRule {
    std::string selector;
    std::map<std::string, std::string> declarations;

    std::string to_css() const {
        std::ostringstream out;
        out << selector << " {";
        bool first = true;
        for (auto const& [prop, value] : declarations) {
            if (!first) out << " ";
            out << prop << ": " << value << ";";
            first = false;
        }
        out << " }";
        return out.str();
    }
};

/** CSS object model: ordered rule list with CSS text emission. */
class Stylesheet {
public:
    Stylesheet& rule(CssRule value) {
        rules_.push_back(std::move(value));
        return *this;
    }
    Stylesheet& rule(std::string selector,
                      std::map<std::string, std::string> declarations) {
        return rule(CssRule{std::move(selector), std::move(declarations)});
    }

    std::vector<CssRule> const& rules() const { return rules_; }

    std::string to_css() const {
        std::ostringstream out;
        for (auto const& rule : rules_) out << rule.to_css() << "\n";
        return out.str();
    }

    /** Default Jiterati manual theme. */
    static Stylesheet default_theme() {
        Stylesheet sheet;
        sheet.rule("article", {{"max-width", "72ch"}, {"margin", "0 auto"}});
        sheet.rule("code", {{"font-family", "monospace"}});
        sheet.rule("pre", {{"background", "#f6f6f6"}, {"padding", "1em"}});
        return sheet;
    }

private:
    std::vector<CssRule> rules_;
};

} // namespace jiterati::docir

#endif // JITERATI_DOCIR_HPP_INCLUDED
