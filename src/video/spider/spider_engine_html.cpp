#include "ttplayer/video/spider/spider_builtins.h"

// 模板 C（HTML 抓取）引擎。
//
// 15 个 csp_* 类都是「GET 页面 + 选择器取字段」的形态，站点差异全部交给
// sources/spiders/csp_*.json 描述。本文件提供三样东西：
//   1) 容错 HTML 解析（不要求闭合；script/style 原样吃文本；处理注释/doctype）；
//   2) TVBox jsoup 风格的 CSS 选择器子集与取值后缀（@text/@href/@attr(x)）；
//   3) recipe 驱动的通用引擎：home/category/search/detail/player + 镜像回落。
//
// 只有真网络跑通（home 或 category 有卡片、detail 有线路）的类才标 implemented。
// 源站关闭/403/过盾/需登录的类保留注册但标 false，原因写在 SpiderMeta::note。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "ttplayer/video/spider/spider_http.h"
#include "ttplayer/video/spider/spider_recipe.h"
#include "ttplayer/video/spider/spider_registry.h"
#include "ttplayer/video/spider/spider_util.h"

namespace ttplayer::video {

namespace {

namespace su = spider_util;
using nlohmann::json;

// ============================ 容错 HTML DOM ============================

struct HtmlNode {
    std::string tag;                              // 小写；文本节点为 "#text"
    std::map<std::string, std::string> attrs;     // 键小写，值已解码实体
    std::string text;                             // 仅 "#text" 使用，已解码实体
    std::vector<std::unique_ptr<HtmlNode>> children;
    HtmlNode* parent = nullptr;
    int order = 0;

    bool IsElement() const { return !tag.empty() && tag.front() != '#'; }

    const std::string& Attr(const std::string& name) const {
        const auto it = attrs.find(name);
        return it == attrs.end() ? Empty() : it->second;
    }
    bool HasAttr(const std::string& name) const {
        return attrs.find(name) != attrs.end();
    }

    static const std::string& Empty() {
        static const std::string empty;
        return empty;
    }
};

bool IsVoidTag(const std::string& tag) {
    static const char* kVoid[] = {"area", "base", "br",    "col",   "embed", "hr",
                                  "img",  "input", "link", "meta",  "param", "source",
                                  "track", "wbr"};
    for (const char* item : kVoid)
        if (tag == item) return true;
    return false;
}

bool IsRawTextTag(const std::string& tag) {
    return tag == "script" || tag == "style";
}

std::string CollapseWhitespace(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(raw);
    }
    return out;
}

void CollectText(const HtmlNode& node, std::string& out) {
    if (node.tag == "#text") {
        out += node.text;
        return;
    }
    if (node.tag == "script" || node.tag == "style") return;
    for (const auto& child : node.children) {
        std::string piece;
        CollectText(*child, piece);
        if (piece.empty()) continue;
        if (!out.empty() && !std::isspace(static_cast<unsigned char>(out.back())))
            out.push_back(' ');
        out += piece;
    }
}

std::string NodeText(const HtmlNode& node) {
    std::string raw;
    CollectText(node, raw);
    return su::Trim(CollapseWhitespace(raw));
}

class HtmlParser {
public:
    explicit HtmlParser(std::string_view html) : s_(html) {}

    HtmlNode Parse() {
        root_.tag = "#root";
        root_.parent = nullptr;
        root_.order = next_order_++;
        stack_.push_back(&root_);

        const std::size_t n = s_.size();
        std::size_t i = 0;
        while (i < n) {
            const std::size_t lt = s_.find('<', i);
            if (lt == std::string_view::npos) {
                AddText(s_.substr(i));
                break;
            }
            if (lt > i) AddText(s_.substr(i, lt - i));

            if (StartsWith(lt, "<!--")) {
                const std::size_t end = s_.find("-->", lt + 4);
                i = end == std::string_view::npos ? n : end + 3;
                continue;
            }
            if (StartsWith(lt, "<!")) {
                const std::size_t end = s_.find('>', lt + 2);
                i = end == std::string_view::npos ? n : end + 1;
                continue;
            }
            if (StartsWith(lt, "</")) {
                const std::size_t end = s_.find('>', lt + 2);
                if (end == std::string_view::npos) break;
                CloseTag(su::Trim(s_.substr(lt + 2, end - lt - 2)));
                i = end + 1;
                continue;
            }
            // 普通文本里出现 "< 3" 这类内容时按文本处理。
            if (lt + 1 >= n || !IsTagNameStart(s_[lt + 1])) {
                AddText("<");
                i = lt + 1;
                continue;
            }

            const std::size_t end = FindTagEnd(lt + 1);
            if (end == std::string_view::npos) {
                AddText(s_.substr(lt));
                break;
            }
            const std::string_view inner = s_.substr(lt + 1, end - lt - 1);
            ParseStartTag(inner);
            i = end + 1;

            if (!stack_.empty() && IsRawTextTag(stack_.back()->tag)) {
                const std::string closing = "</" + stack_.back()->tag;
                const std::size_t close = FindCloseTag(i, closing);
                const std::size_t text_end =
                    close == std::string_view::npos ? n : close;
                AddText(s_.substr(i, text_end - i));
                if (close == std::string_view::npos) {
                    i = n;
                } else {
                    const std::size_t gt = s_.find('>', close);
                    i = gt == std::string_view::npos ? n : gt + 1;
                }
                stack_.pop_back();
            }
        }
        return std::move(root_);
    }

private:
    bool StartsWith(std::size_t pos, std::string_view prefix) const {
        return s_.substr(pos, prefix.size()) == prefix;
    }
    static bool IsTagNameStart(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '/' || c == '!' ||
               c == '?';
    }
    std::size_t FindTagEnd(std::size_t from) const {
        char quote = 0;
        for (std::size_t i = from; i < s_.size(); ++i) {
            const char c = s_[i];
            if (quote) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                continue;
            }
            if (c == '>') return i;
        }
        return std::string_view::npos;
    }
    std::size_t FindCloseTag(std::size_t from, const std::string& lower_closing) const {
        const std::string hay = su::ToLower(std::string(s_.substr(from)));
        const std::size_t pos = hay.find(lower_closing);
        return pos == std::string::npos ? std::string_view::npos : from + pos;
    }

    void AddText(std::string_view raw) {
        if (raw.empty()) return;
        auto node = std::make_unique<HtmlNode>();
        node->tag = "#text";
        node->text = su::HtmlDecode(raw);
        node->order = next_order_++;
        node->parent = stack_.back();
        stack_.back()->children.push_back(std::move(node));
    }

    void CloseTag(const std::string& raw_name) {
        std::string name = su::ToLower(raw_name);
        const std::size_t space = name.find_first_of(" \t\r\n");
        if (space != std::string::npos) name.resize(space);
        if (name.empty()) return;
        for (std::size_t i = stack_.size(); i-- > 1;) {
            if (stack_[i]->tag == name) {
                stack_.resize(i);
                return;
            }
        }
    }

    void ParseStartTag(std::string_view inner) {
        std::size_t i = 0;
        const std::size_t n = inner.size();
        while (i < n && std::isspace(static_cast<unsigned char>(inner[i]))) ++i;
        const std::size_t name_start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(inner[i])) &&
               inner[i] != '/' && inner[i] != '>')
            ++i;
        std::string tag = su::ToLower(inner.substr(name_start, i - name_start));
        if (tag.empty()) return;

        bool self_closing = false;
        {
            std::size_t back = inner.size();
            while (back > 0 && std::isspace(static_cast<unsigned char>(inner[back - 1])))
                --back;
            if (back > 0 && inner[back - 1] == '/') self_closing = true;
        }

        auto node = std::make_unique<HtmlNode>();
        node->tag = tag;
        node->order = next_order_++;
        ParseAttributes(inner.substr(i), node.get());

        HtmlNode* raw = node.get();
        raw->parent = stack_.back();
        stack_.back()->children.push_back(std::move(node));

        if (self_closing || IsVoidTag(tag)) return;
        stack_.push_back(raw);
    }

    static void ParseAttributes(std::string_view text, HtmlNode* node) {
        std::size_t i = 0;
        const std::size_t n = text.size();
        while (i < n) {
            while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            if (i >= n) break;
            if (text[i] == '/' || text[i] == '>') {
                ++i;
                continue;
            }
            const std::size_t name_start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) &&
                   text[i] != '=' && text[i] != '/' && text[i] != '>')
                ++i;
            if (i == name_start) {
                ++i;
                continue;
            }
            const std::string name =
                su::ToLower(text.substr(name_start, i - name_start));
            while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;

            std::string value;
            if (i < n && text[i] == '=') {
                ++i;
                while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                if (i < n && (text[i] == '"' || text[i] == '\'')) {
                    const char quote = text[i++];
                    const std::size_t value_start = i;
                    while (i < n && text[i] != quote) ++i;
                    value = su::HtmlDecode(text.substr(value_start, i - value_start));
                    if (i < n) ++i;
                } else {
                    const std::size_t value_start = i;
                    while (i < n && !std::isspace(static_cast<unsigned char>(text[i])) &&
                           text[i] != '/' && text[i] != '>')
                        ++i;
                    value = su::HtmlDecode(text.substr(value_start, i - value_start));
                }
            }
            if (node->attrs.find(name) == node->attrs.end()) node->attrs[name] = value;
        }
    }

    std::string_view s_;
    HtmlNode root_;
    std::vector<HtmlNode*> stack_;
    int next_order_ = 0;
};

// ============================ CSS 选择器子集 ============================

struct AttrSelector {
    std::string name;
    std::string op;  // "" / "=" / "*=" / "^=" / "$=" / "~=" / "!="
    std::string value;
};

struct SimpleSelector {
    std::string tag;  // 空或 "*" 表示任意
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttrSelector> attrs;
    std::string contains;
    int eq = -1;
};

struct SelectorStep {
    char combinator = ' ';
    SimpleSelector selector;
};

struct SelectorExpr {
    std::vector<std::vector<SelectorStep>> groups;
    std::string suffix;  // "text" / "html" / "href" / "attr(x)" / 属性名
};

bool IsSpaceChar(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

bool IsIdentChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '-' || c == '_' || u >= 0x80;
}

bool ParseAttrSelector(std::string_view body, AttrSelector* out) {
    std::string text = su::Trim(body);
    if (text.empty()) return false;
    static const char* kOps[] = {"*=", "^=", "$=", "~=", "!=", "|=", "="};
    std::size_t op_pos = std::string::npos;
    std::string op;
    for (const char* candidate : kOps) {
        const std::size_t pos = text.find(candidate);
        if (pos != std::string::npos) {
            op_pos = pos;
            op = candidate;
            break;
        }
    }
    if (op_pos == std::string::npos) {
        out->name = su::ToLower(text);
        return !out->name.empty();
    }
    out->name = su::ToLower(su::Trim(text.substr(0, op_pos)));
    std::string value = su::Trim(text.substr(op_pos + op.size()));
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    out->value = value;
    if (op == "|=") op = "=";
    out->op = op;
    return !out->name.empty();
}

bool ParseSimpleSelector(std::string_view text, SimpleSelector* out) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    bool any = false;
    while (i < n) {
        const char c = text[i];
        if (c == '*') {
            ++i;
            any = true;
            continue;
        }
        if (c == '.') {
            ++i;
            const std::size_t start = i;
            while (i < n && IsIdentChar(text[i])) ++i;
            if (i > start) out->classes.push_back(std::string(text.substr(start, i - start)));
            any = true;
            continue;
        }
        if (c == '#') {
            ++i;
            const std::size_t start = i;
            while (i < n && IsIdentChar(text[i])) ++i;
            if (i > start) out->id = std::string(text.substr(start, i - start));
            any = true;
            continue;
        }
        if (c == '[') {
            const std::size_t close = text.find(']', i + 1);
            if (close == std::string_view::npos) break;
            AttrSelector attr;
            if (ParseAttrSelector(text.substr(i + 1, close - i - 1), &attr))
                out->attrs.push_back(std::move(attr));
            i = close + 1;
            any = true;
            continue;
        }
        if (c == ':') {
            ++i;
            const std::size_t name_start = i;
            while (i < n && (std::isalpha(static_cast<unsigned char>(text[i])) ||
                             text[i] == '-'))
                ++i;
            const std::string name =
                su::ToLower(text.substr(name_start, i - name_start));
            std::string arg;
            if (i < n && text[i] == '(') {
                ++i;
                const std::size_t arg_start = i;
                int depth = 1;
                while (i < n && depth > 0) {
                    if (text[i] == '(') ++depth;
                    if (text[i] == ')') --depth;
                    if (depth == 0) break;
                    ++i;
                }
                arg = su::Trim(text.substr(arg_start, i - arg_start));
                if (i < n) ++i;
            }
            if (name == "eq") {
                try {
                    out->eq = std::stoi(arg);
                } catch (...) {
                    out->eq = -1;
                }
            } else if (name == "contains") {
                if (arg.size() >= 2 && (arg.front() == '"' || arg.front() == '\'') &&
                    arg.back() == arg.front())
                    arg = arg.substr(1, arg.size() - 2);
                out->contains = arg;
            }
            any = true;
            continue;
        }
        if (IsIdentChar(c)) {
            const std::size_t start = i;
            while (i < n && IsIdentChar(text[i])) ++i;
            if (out->tag.empty())
                out->tag = su::ToLower(text.substr(start, i - start));
            any = true;
            continue;
        }
        ++i;
    }
    return any;
}

std::vector<std::vector<SelectorStep>> ParseSelectorGroups(std::string_view text) {
    std::vector<std::vector<SelectorStep>> groups;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        int depth = 0;
        char quote = 0;
        std::size_t end = text.size();
        for (std::size_t i = pos; i < text.size(); ++i) {
            const char c = text[i];
            if (quote) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                continue;
            }
            if (c == '[' || c == '(') ++depth;
            if (c == ']' || c == ')') --depth;
            if (c == ',' && depth <= 0) {
                end = i;
                break;
            }
        }
        const std::string_view group_text = text.substr(pos, end - pos);

        std::vector<SelectorStep> steps;
        std::size_t i = 0;
        const std::size_t n = group_text.size();
        char combinator = ' ';
        while (i < n) {
            bool saw_space = false;
            while (i < n && IsSpaceChar(group_text[i])) {
                saw_space = true;
                ++i;
            }
            if (i < n && group_text[i] == '>') {
                combinator = '>';
                ++i;
                while (i < n && IsSpaceChar(group_text[i])) ++i;
            } else if (!steps.empty() && saw_space) {
                combinator = ' ';
            }
            if (i >= n) break;

            const std::size_t compound_start = i;
            int compound_depth = 0;
            char compound_quote = 0;
            while (i < n) {
                const char c = group_text[i];
                if (compound_quote) {
                    if (c == compound_quote) compound_quote = 0;
                    ++i;
                    continue;
                }
                if (c == '"' || c == '\'') {
                    compound_quote = c;
                    ++i;
                    continue;
                }
                if (c == '[' || c == '(') ++compound_depth;
                if (c == ']' || c == ')') --compound_depth;
                if (compound_depth <= 0 && (IsSpaceChar(c) || c == '>')) break;
                ++i;
            }
            SimpleSelector selector;
            if (ParseSimpleSelector(group_text.substr(compound_start,
                                                      i - compound_start),
                                    &selector)) {
                SelectorStep step;
                step.combinator = steps.empty() ? ' ' : combinator;
                step.selector = std::move(selector);
                steps.push_back(std::move(step));
            }
            combinator = ' ';
        }
        if (!steps.empty()) groups.push_back(std::move(steps));
        if (end >= text.size()) break;
        pos = end + 1;
    }
    return groups;
}

SelectorExpr ParseSelectorExpr(std::string_view raw) {
    SelectorExpr expr;
    const std::string text = su::Trim(raw);
    std::size_t at = std::string::npos;
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '[' || c == '(') ++depth;
        if (c == ']' || c == ')') --depth;
        if (c == '@' && depth <= 0) at = i;
    }
    if (at == std::string::npos) {
        expr.groups = ParseSelectorGroups(text);
        return expr;
    }
    expr.suffix = su::Trim(text.substr(at + 1));
    expr.groups = ParseSelectorGroups(text.substr(0, at));
    return expr;
}

std::set<std::string> SplitClasses(const std::string& text) {
    std::set<std::string> classes;
    for (const std::string& part : su::Split(text, ' '))
        if (!part.empty()) classes.insert(part);
    // 兼容制表符/换行分隔。
    if (classes.empty()) {
        std::string current;
        for (char c : text) {
            if (IsSpaceChar(c)) {
                if (!current.empty()) classes.insert(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) classes.insert(current);
    }
    return classes;
}

bool AttrMatches(const std::string& value, const AttrSelector& attr) {
    if (attr.op.empty()) return true;
    if (attr.op == "=") return value == attr.value;
    if (attr.op == "*=") return value.find(attr.value) != std::string::npos;
    if (attr.op == "^=") return value.rfind(attr.value, 0) == 0;
    if (attr.op == "$=") {
        if (value.size() < attr.value.size()) return false;
        return value.compare(value.size() - attr.value.size(), attr.value.size(),
                             attr.value) == 0;
    }
    if (attr.op == "~=") {
        std::string current;
        for (char c : value) {
            if (IsSpaceChar(c)) {
                if (current == attr.value) return true;
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        return current == attr.value;
    }
    if (attr.op == "!=") return value != attr.value;
    return true;
}

bool NodeMatches(const HtmlNode& node, const SimpleSelector& selector) {
    if (!node.IsElement()) return false;
    if (!selector.tag.empty() && selector.tag != "*" && node.tag != selector.tag)
        return false;
    if (!selector.id.empty() && node.Attr("id") != selector.id) return false;
    if (!selector.classes.empty()) {
        const std::set<std::string> classes = SplitClasses(node.Attr("class"));
        for (const std::string& name : selector.classes)
            if (classes.find(name) == classes.end()) return false;
    }
    for (const AttrSelector& attr : selector.attrs) {
        const auto it = node.attrs.find(attr.name);
        if (it == node.attrs.end()) return false;
        if (!AttrMatches(it->second, attr)) return false;
    }
    if (!selector.contains.empty()) {
        if (NodeText(node).find(selector.contains) == std::string::npos) return false;
    }
    return true;
}

void CollectDescendants(const HtmlNode& node, bool include_self,
                        std::vector<const HtmlNode*>* out) {
    if (include_self && node.IsElement()) out->push_back(&node);
    for (const auto& child : node.children)
        CollectDescendants(*child, true, out);
}

std::vector<const HtmlNode*> EvalSelector(const HtmlNode& scope,
                                          const SelectorExpr& expr) {
    std::vector<const HtmlNode*> result;
    for (const auto& steps : expr.groups) {
        std::vector<const HtmlNode*> current;
        current.push_back(&scope);
        for (std::size_t step_index = 0; step_index < steps.size(); ++step_index) {
            const SelectorStep& step = steps[step_index];
            std::vector<const HtmlNode*> matched;
            for (const HtmlNode* node : current) {
                if (!node) continue;
                if (step.combinator == '>' && step_index > 0) {
                    for (const auto& child : node->children) {
                        if (child->IsElement() &&
                            NodeMatches(*child, step.selector))
                            matched.push_back(child.get());
                    }
                } else {
                    std::vector<const HtmlNode*> candidates;
                    CollectDescendants(*node, step_index == 0, &candidates);
                    for (const HtmlNode* candidate : candidates)
                        if (NodeMatches(*candidate, step.selector))
                            matched.push_back(candidate);
                }
            }
            std::sort(matched.begin(), matched.end(),
                      [](const HtmlNode* a, const HtmlNode* b) {
                          return a->order < b->order;
                      });
            matched.erase(std::unique(matched.begin(), matched.end()),
                          matched.end());
            if (step.selector.eq >= 0) {
                const std::size_t index =
                    static_cast<std::size_t>(step.selector.eq);
                std::vector<const HtmlNode*> picked;
                if (index < matched.size()) picked.push_back(matched[index]);
                matched.swap(picked);
            }
            current.swap(matched);
            if (current.empty()) break;
        }
        result.insert(result.end(), current.begin(), current.end());
    }
    std::sort(result.begin(), result.end(),
              [](const HtmlNode* a, const HtmlNode* b) { return a->order < b->order; });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void SerializeInner(const HtmlNode& node, std::string& out) {
    for (const auto& child : node.children) {
        if (child->tag == "#text") {
            out += child->text;
            continue;
        }
        out += "<" + child->tag;
        for (const auto& kv : child->attrs) {
            out += " " + kv.first;
            if (!kv.second.empty()) out += "=\"" + kv.second + "\"";
        }
        out += ">";
        SerializeInner(*child, out);
        out += "</" + child->tag + ">";
    }
}

std::string ExtractValue(const std::string& suffix, const HtmlNode& node) {
    const std::string key = su::ToLower(su::Trim(suffix));
    if (key.empty() || key == "text") return NodeText(node);
    if (key == "html") {
        std::string out;
        SerializeInner(node, out);
        return su::Trim(out);
    }
    std::string attr_name = key;
    if (key.rfind("attr(", 0) == 0 && key.back() == ')')
        attr_name = su::Trim(key.substr(5, key.size() - 6));
    return su::Trim(node.Attr(su::ToLower(attr_name)));
}

std::vector<const HtmlNode*> SelectNodes(const HtmlNode& scope,
                                         const std::string& selector) {
    const SelectorExpr expr = ParseSelectorExpr(selector);
    if (expr.groups.empty()) return {};
    return EvalSelector(scope, expr);
}

std::string SelectOne(const HtmlNode& scope, const std::string& selector) {
    if (su::Trim(selector).empty()) return {};
    const SelectorExpr expr = ParseSelectorExpr(selector);
    if (expr.groups.empty()) return {};
    const std::vector<const HtmlNode*> nodes = EvalSelector(scope, expr);
    if (nodes.empty()) return {};
    return ExtractValue(expr.suffix, *nodes.front());
}

std::vector<std::string> SelectAll(const HtmlNode& scope,
                                   const std::string& selector) {
    std::vector<std::string> out;
    if (su::Trim(selector).empty()) return out;
    const SelectorExpr expr = ParseSelectorExpr(selector);
    if (expr.groups.empty()) return out;
    for (const HtmlNode* node : EvalSelector(scope, expr)) {
        const std::string value = ExtractValue(expr.suffix, *node);
        if (!value.empty()) out.push_back(value);
    }
    return out;
}

// ---------------------------- 选择器自检 ----------------------------

bool SelfTestSelector() {
    static const char* kHtml =
        "<div class=\"box item\" id=\"main\" data-k=\"v1\">"
        "<a href=\"/a/1.html\" title=\"T1\">甲</a><span class=\"tag\">乙</span>"
        "</div>"
        "<div class=\"box\"><a href=\"/a/2.html\">丙</a></div>";
    HtmlNode doc = HtmlParser(kHtml).Parse();

    bool ok = true;
    ok = ok && SelectNodes(doc, ".box").size() == 2;                    // .class
    ok = ok && SelectNodes(doc, "[data-k*=v1]").size() == 1;            // [attr*=]
    ok = ok && SelectOne(doc, ".box:eq(1) a@href") == "/a/2.html";      // :eq + @attr
    ok = ok && SelectOne(doc, "div.box > a@text") == "甲";              // 子代 + @text
    ok = ok && SelectOne(doc, "[href*=2.html]@href") == "/a/2.html";    // [attr*=]
    ok = ok && SelectOne(doc, ":contains(丙)") == "丙";                  // :contains
    ok = ok && SelectOne(doc, "#main@data-k") == "v1";                  // #id + @attr
    std::printf("HTML_SELFTEST: %s (class/attr/attr*/eq/child/text/contains/id)\n",
                ok ? "PASS" : "FAIL");
    return ok;
}

// ============================ 编码与 URL ============================

bool LooksLikeUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
            extra = 1;
        else if ((c & 0xF0) == 0xE0)
            extra = 2;
        else if ((c & 0xF8) == 0xF0)
            extra = 3;
        else
            return false;
        if (i + static_cast<std::size_t>(extra) >= text.size()) return true;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

std::string ConvertToUtf8(std::string_view bytes, UINT codepage) {
    if (bytes.empty()) return {};
    const int wide_len = MultiByteToWideChar(
        codepage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_len <= 0) return std::string(bytes);
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(codepage, 0, bytes.data(), static_cast<int>(bytes.size()),
                        wide.data(), wide_len);
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len,
                                             nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return std::string(bytes);
    std::string out(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, out.data(), utf8_len,
                        nullptr, nullptr);
    return out;
}

std::string CharsetFromHeaders(
    const std::map<std::string, std::vector<std::string>>& headers) {
    for (const auto& kv : headers) {
        if (su::ToLower(kv.first) != "content-type") continue;
        for (const std::string& value : kv.second) {
            const std::string lower = su::ToLower(value);
            const std::size_t pos = lower.find("charset=");
            if (pos == std::string::npos) continue;
            std::size_t start = pos + 8;
            std::size_t end = start;
            while (end < lower.size() && !IsSpaceChar(lower[end]) &&
                   lower[end] != ';' && lower[end] != '"' && lower[end] != '\'')
                ++end;
            return lower.substr(start, end - start);
        }
    }
    return {};
}

std::string CharsetFromMeta(std::string_view body) {
    const std::string head = su::ToLower(std::string(body.substr(0, 4096)));
    std::size_t pos = head.find("charset=");
    if (pos == std::string::npos) return {};
    pos += 8;
    std::size_t end = pos;
    while (end < head.size() && !IsSpaceChar(head[end]) && head[end] != ';' &&
           head[end] != '"' && head[end] != '\'' && head[end] != '>')
        ++end;
    return head.substr(pos, end - pos);
}

std::string EnsureUtf8(
    std::string body, const std::map<std::string, std::vector<std::string>>& headers) {
    std::string charset = CharsetFromHeaders(headers);
    if (charset.empty()) charset = CharsetFromMeta(body);

    if (!charset.empty()) {
        if (charset.find("utf-8") != std::string::npos ||
            charset.find("utf8") != std::string::npos)
            return body;
        if (charset.find("gb") != std::string::npos ||
            charset.find("gbk") != std::string::npos ||
            charset.find("gb2312") != std::string::npos)
            return ConvertToUtf8(body, 936);
        if (charset.find("big5") != std::string::npos) return ConvertToUtf8(body, 950);
    }
    if (LooksLikeUtf8(body)) return body;
    return ConvertToUtf8(body, 936);
}

bool IsHttpUrl(std::string_view text) {
    const std::string lower = su::ToLower(text);
    return su::StartsWith(lower, "http://") || su::StartsWith(lower, "https://");
}

std::string NormalizeBase(std::string base) {
    base = su::Trim(base);
    if (base.empty()) return base;
    if (su::StartsWith(base, "//")) return "http:" + base;
    if (!IsHttpUrl(base)) return "http://" + base;
    return base;
}

std::string NormalizeUrl(const std::string& base, std::string value) {
    value = su::Trim(value);
    if (value.empty()) return value;
    if (IsHttpUrl(value)) return value;
    if (su::StartsWith(value, "//")) return "https:" + value;
    if (value.front() == '/') return su::JoinUrl(base, value);
    return su::JoinUrl(base, value);
}

bool IsDirectMedia(std::string_view url) {
    const std::string lower = su::ToLower(url);
    static const char* kExts[] = {".m3u8", ".mp4",  ".flv", ".ts",  ".mkv",
                                  ".avi",  ".mp3",  ".m4a", ".wav", ".ogg"};
    for (const char* ext : kExts)
        if (lower.find(ext) != std::string::npos) return true;
    return false;
}

bool IsNonPageScheme(std::string_view url) {
    const std::string lower = su::ToLower(url);
    if (su::StartsWith(lower, "magnet:") || su::StartsWith(lower, "ed2k:") ||
        su::StartsWith(lower, "thunder:") || su::StartsWith(lower, "javascript:"))
        return true;
    static const char* kPanHosts[] = {"pan.quark.cn",   "pan.baidu.com",  "www.aliyundrive",
                                      "www.alipan.com", "drive.uc.cn",    "115.com",
                                      "yun.139.com",    "caiyun.139.com", "cloud.189.cn"};
    for (const char* host : kPanHosts)
        if (lower.find(host) != std::string::npos) return true;
    return false;
}

// ============================ recipe 驱动 ============================

std::string JsonStr(const json& obj, const char* key, std::string_view def = {}) {
    if (!obj.is_object()) return std::string(def);
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::string(def);
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_boolean()) return it->get<bool>() ? "1" : "0";
    return std::string(def);
}

std::string JsonScalar(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_boolean()) return value.get<bool>() ? "1" : "0";
    return {};
}

json ParseExtObject(std::string_view ext) {
    const std::string text = su::Trim(ext);
    if (text.empty()) return json::object();
    const json parsed = su::ParseJsonLoose(text);
    if (parsed.is_object()) return parsed;
    if (parsed.is_string()) {
        const std::string inner = parsed.get<std::string>();
        if (!inner.empty()) return json{{"base", inner}};
        return json::object();
    }
    return json{{"base", text}};
}

struct HtmlProfile {
    json recipe;
    json ext = json::object();
    std::string base;
    std::vector<std::string> mirrors;
    std::map<std::string, std::string> headers;
};

std::mutex g_base_mutex;
std::map<std::string, std::string> g_working_base;

void MergeHeaders(const json& source, std::map<std::string, std::string>* headers) {
    if (!headers || !source.is_object()) return;
    const auto it = source.find("headers");
    if (it == source.end() || !it->is_object()) return;
    for (auto header = it->begin(); header != it->end(); ++header)
        if (header.value().is_string())
            (*headers)[header.key()] = header.value().get<std::string>();
}

void CollectMirrors(const json& value, std::vector<std::string>* out) {
    if (!value.is_array() || !out) return;
    for (const auto& entry : value) {
        if (!entry.is_string()) continue;
        const std::string mirror = NormalizeBase(entry.get<std::string>());
        if (mirror.empty()) continue;
        if (std::find(out->begin(), out->end(), mirror) == out->end())
            out->push_back(mirror);
    }
}

HtmlProfile BuildProfile(const SpiderContext& ctx) {
    HtmlProfile profile;
    const SpiderRecipeLoad load = LoadSpiderRecipe(ctx.spider_id.empty()
                                                       ? ctx.site.api
                                                       : ctx.spider_id);
    if (!load.ok) {
        throw std::runtime_error("模板 C 缺少 recipe：" +
                                 (load.error.empty() ? ctx.spider_id : load.error));
    }
    profile.recipe = load.recipe.raw;
    profile.ext = ParseExtObject(ctx.ext.empty() ? ctx.site.ext : ctx.ext);
    profile.base = NormalizeBase(load.recipe.base);

    // ext 里的 base/host/siteUrl/url 优先（用户可覆盖镜像）。
    if (profile.ext.is_object()) {
        for (const char* key : {"base", "host", "siteUrl", "site_url", "url"}) {
            const auto it = profile.ext.find(key);
            if (it == profile.ext.end() || !it->is_string()) continue;
            const std::string value = NormalizeBase(it->get<std::string>());
            if (!value.empty()) {
                profile.base = value;
                break;
            }
        }
    }

    CollectMirrors(profile.recipe.value("mirrors", json()), &profile.mirrors);
    if (profile.recipe.contains("html"))
        CollectMirrors(profile.recipe["html"].value("mirrors", json()),
                       &profile.mirrors);
    profile.mirrors.erase(
        std::remove(profile.mirrors.begin(), profile.mirrors.end(), profile.base),
        profile.mirrors.end());

    {
        std::lock_guard<std::mutex> lock(g_base_mutex);
        const std::string key = ctx.spider_id + "|" + profile.base;
        const auto it = g_working_base.find(key);
        if (it != g_working_base.end()) profile.base = it->second;
    }

    MergeHeaders(profile.recipe, &profile.headers);
    MergeHeaders(profile.ext, &profile.headers);
    bool has_accept = false;
    bool has_ua = false;
    for (const auto& kv : profile.headers) {
        const std::string lower = su::ToLower(kv.first);
        if (lower == "accept") has_accept = true;
        if (lower == "user-agent") has_ua = true;
    }
    if (!has_accept)
        profile.headers["Accept"] =
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
    if (!has_ua)
        profile.headers["User-Agent"] =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/122.0.6299.95 Safari/537.36";
    return profile;
}

std::string RebaseUrl(const std::string& url, const std::string& from,
                      const std::string& to) {
    if (from.empty()) return url;
    if (url.rfind(from, 0) == 0) return to + url.substr(from.size());
    const std::size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const std::size_t slash = url.find('/', scheme + 3);
        const std::string path =
            slash == std::string::npos ? "/" : url.substr(slash);
        return su::JoinUrl(to, path);
    }
    return url;
}

struct HtmlRun {
    const SpiderContext* ctx = nullptr;
    HtmlProfile profile;
    std::string cache_key;
    std::unique_ptr<SpiderHttp> http;
};

HtmlRun BeginRun(const SpiderContext& ctx) {
    HtmlRun run;
    run.ctx = &ctx;
    run.profile = BuildProfile(ctx);
    run.cache_key = ctx.spider_id + "|" + run.profile.base;
    run.http = std::make_unique<SpiderHttp>(ctx.spider_id, ctx.net, ctx.site);
    run.http->SetDefaultHeaders(run.profile.headers);
    return run;
}

std::string FetchPage(HtmlRun& run, const std::string& url, const char* stage) {
    std::vector<std::string> candidates;
    candidates.push_back(url);
    for (const std::string& mirror : run.profile.mirrors) {
        const std::string candidate = RebaseUrl(url, run.profile.base, mirror);
        if (candidate != url &&
            std::find(candidates.begin(), candidates.end(), candidate) ==
                candidates.end())
            candidates.push_back(candidate);
    }

    run.http->SetStage(stage);
    std::string last_error;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        try {
            const SpiderHttpResponse response = run.http->Get(candidates[i]);
            if (response.status >= 400 || response.body.empty()) {
                last_error = "status=" + std::to_string(response.status);
                continue;
            }
            std::string body = EnsureUtf8(response.body, response.headers);
            if (body.empty()) {
                last_error = "空响应";
                continue;
            }
            if (i > 0) {
                run.profile.base = run.profile.mirrors[i - 1];
                std::lock_guard<std::mutex> lock(g_base_mutex);
                g_working_base[run.cache_key] = run.profile.base;
            }
            return body;
        } catch (const std::exception& e) {
            last_error = e.what();
        }
    }
    throw std::runtime_error("HTML 请求失败：" + MaskSpiderUrl(url) + " " +
                             last_error);
}

std::string EndpointPath(const json& recipe, const char* name) {
    const auto endpoints = recipe.find("endpoints");
    if (endpoints == recipe.end() || !endpoints->is_object()) return {};
    const auto endpoint = endpoints->find(name);
    if (endpoint == endpoints->end() || !endpoint->is_object()) return {};
    return JsonStr(*endpoint, "path");
}

std::string ExpandVars(std::string text, const std::map<std::string, std::string>& vars) {
    for (const auto& kv : vars) {
        if (kv.first.empty()) continue;
        text = su::ReplaceAll(std::move(text), "{" + kv.first + "}", kv.second);
    }
    return text;
}

std::string BuildRequestUrl(const HtmlRun& run, const std::string& path,
                            const json& params,
                            const std::map<std::string, std::string>& vars) {
    std::string raw = ExpandVars(su::Trim(path), vars);
    if (raw.empty()) return {};
    std::string url;
    if (IsHttpUrl(raw)) {
        url = raw;
    } else if (!raw.empty() && raw.front() == '?') {
        std::string base = run.profile.base;
        while (!base.empty() && base.back() == '/') base.pop_back();
        url = base + raw;
    } else {
        url = su::JoinUrl(run.profile.base, raw);
    }

    if (params.is_object()) {
        const bool has_query = url.find('?') != std::string::npos;
        bool first = !has_query;
        for (auto it = params.begin(); it != params.end(); ++it) {
            std::string value;
            if (it.value().is_string()) {
                const std::string token = it.value().get<std::string>();
                auto found = vars.find(token);
                if (found != vars.end())
                    value = found->second;
                else if (run.profile.ext.is_object() &&
                         run.profile.ext.contains(token))
                    value = JsonScalar(run.profile.ext.at(token));
                else if (token.find('{') != std::string::npos)
                    value = ExpandVars(token, vars);
                else
                    value = token;
            } else {
                value = JsonScalar(it.value());
            }
            if (value.empty()) continue;
            url += first ? "?" : "&";
            first = false;
            url += su::UrlEncode(it.key()) + "=" + su::UrlEncode(value);
        }
    }
    return url;
}

std::string EndpointUrl(const HtmlRun& run, const char* name,
                        const std::map<std::string, std::string>& vars) {
    const json& recipe = run.profile.recipe;
    const std::string path = EndpointPath(recipe, name);
    json params = json::object();
    const auto endpoints = recipe.find("endpoints");
    if (endpoints != recipe.end() && endpoints->is_object()) {
        const auto endpoint = endpoints->find(name);
        if (endpoint != endpoints->end() && endpoint->is_object())
            params = endpoint->value("params", json::object());
    }
    return BuildRequestUrl(run, path, params, vars);
}

std::string FieldValue(const HtmlNode& scope, const json& fields, const char* key) {
    if (!fields.is_object()) return {};
    const auto it = fields.find(key);
    if (it == fields.end()) return {};
    if (it->is_string()) return SelectOne(scope, it->get<std::string>());
    if (it->is_array()) {
        for (const auto& entry : *it) {
            if (!entry.is_string()) continue;
            const std::string value = SelectOne(scope, entry.get<std::string>());
            if (!value.empty()) return value;
        }
    }
    return {};
}

std::vector<std::string> FieldValues(const HtmlNode& scope, const json& fields,
                                     const char* key) {
    if (!fields.is_object()) return {};
    const auto it = fields.find(key);
    if (it == fields.end()) return {};
    if (it->is_string()) return SelectAll(scope, it->get<std::string>());
    if (it->is_array()) {
        for (const auto& entry : *it) {
            if (!entry.is_string()) continue;
            std::vector<std::string> values =
                SelectAll(scope, entry.get<std::string>());
            if (!values.empty()) return values;
        }
    }
    return {};
}

std::string SelectorOr(const json& recipe, std::initializer_list<const char*> keys) {
    const auto html = recipe.find("html");
    if (html == recipe.end() || !html->is_object()) return {};
    const auto selectors = html->find("selectors");
    if (selectors == html->end() || !selectors->is_object()) return {};
    for (const char* key : keys) {
        const std::string value = JsonStr(*selectors, key);
        if (!value.empty()) return value;
    }
    return {};
}

VideoListing ParseListingNodes(const HtmlRun& run, const HtmlNode& scope,
                               const std::string& items_selector) {
    VideoListing listing;
    listing.page = 1;
    if (items_selector.empty()) return listing;

    const json extract = run.profile.recipe.value("extract", json::object());
    const json spec = extract.value("listing", json::object());
    const json fields = spec.value("fields", json::object());

    const std::vector<const HtmlNode*> nodes = SelectNodes(scope, items_selector);
    std::set<std::string> seen;
    for (const HtmlNode* node : nodes) {
        if (!node) continue;
        if (listing.items.size() >= 200) break;

        VideoItem item;
        item.id = FieldValue(*node, fields, "vod_id");
        item.title = FieldValue(*node, fields, "vod_name");
        if (item.id.empty()) item.id = SelectOne(*node, "a@href");
        if (item.title.empty()) item.title = SelectOne(*node, "a@title");
        if (item.title.empty()) item.title = NodeText(*node);
        if (item.id.empty() && !item.title.empty()) item.id = item.title;
        if (item.id.empty() || item.title.empty()) continue;
        if (!seen.insert(item.id).second) continue;

        item.pic = FieldValue(*node, fields, "vod_pic");
        if (item.pic.empty()) item.pic = SelectOne(*node, "img@data-src");
        if (item.pic.empty()) item.pic = SelectOne(*node, "img@src");
        item.pic = NormalizeUrl(run.profile.base, item.pic);
        item.remarks = FieldValue(*node, fields, "vod_remarks");
        item.score = FieldValue(*node, fields, "vod_scroe");
        if (item.score.empty()) item.score = FieldValue(*node, fields, "vod_score");
        item.type_name = FieldValue(*node, fields, "vod_class");
        item.year = FieldValue(*node, fields, "vod_year");
        item.area = FieldValue(*node, fields, "vod_area");
        item.actor = FieldValue(*node, fields, "vod_actor");
        item.description = FieldValue(*node, fields, "vod_content");
        item.raw = json{{"href", SelectOne(*node, "a@href")}};
        listing.items.push_back(std::move(item));
    }
    return listing;
}

void AddCategory(std::vector<VideoCategory>* out, std::set<std::string>* seen,
                 std::string id, std::string name) {
    if (!out || !seen) return;
    id = su::Trim(id);
    name = su::Trim(name);
    if (id.empty() || name.empty()) return;
    if (id == "/" || id == "#") return;
    if (!seen->insert(id).second) return;
    VideoCategory category;
    category.id = std::move(id);
    category.name = std::move(name);
    out->push_back(std::move(category));
}

std::vector<VideoCategory> ParseCategories(const HtmlRun& run,
                                           const HtmlNode& doc) {
    std::vector<VideoCategory> categories;
    std::set<std::string> seen;
    const json& recipe = run.profile.recipe;

    const auto explicit_categories = recipe.find("categories");
    if (explicit_categories != recipe.end() && explicit_categories->is_array()) {
        for (const auto& entry : *explicit_categories) {
            if (entry.is_object()) {
                std::string id = JsonStr(entry, "id");
                if (id.empty()) id = JsonStr(entry, "href");
                AddCategory(&categories, &seen, id, JsonStr(entry, "name"));
            } else if (entry.is_string()) {
                const std::string name = entry.get<std::string>();
                AddCategory(&categories, &seen, name, name);
            }
        }
        if (!categories.empty()) return categories;
    }

    const std::string selector =
        SelectorOr(recipe, {"categories", "home_menu", "menu", "cat"});
    if (selector.empty()) return categories;
    for (const HtmlNode* node : SelectNodes(doc, selector)) {
        if (!node || !node->IsElement()) continue;
        std::string href = node->Attr("href");
        if (href.empty()) href = node->Attr("data-href");
        AddCategory(&categories, &seen, href, NodeText(*node));
    }
    return categories;
}

std::string DigitsFrom(std::string_view text) {
    std::string digits = su::DigitsOnly(text);
    return digits;
}

struct PlayLink {
    std::string name;
    std::string url;
};

PlayLink NormalizeEpisode(const std::string& base, std::string name, std::string url,
                          std::size_t index) {
    PlayLink link;
    url = su::Trim(url);
    if (url.empty() || su::StartsWith(url, "javascript:") || url == "#") return link;
    link.url = NormalizeUrl(base, url);
    link.name = su::Trim(name);
    if (link.name.empty()) link.name = "第" + std::to_string(index + 1) + "集";
    return link;
}

void AppendEpisodes(const std::vector<std::string>& names,
                    const std::vector<std::string>& urls, const std::string& base,
                    VideoDetail* detail) {
    if (!detail) return;
    std::vector<PlayLink> links;
    for (std::size_t i = 0; i < urls.size(); ++i)
        links.push_back(NormalizeEpisode(base, i < names.size() ? names[i] : "",
                                         urls[i], i));

    // 去掉 javascript:/# 之类的无效地址。
    links.erase(std::remove_if(links.begin(), links.end(),
                               [](const PlayLink& link) { return link.url.empty(); }),
                links.end());
    if (links.empty()) return;

    const std::size_t name_count = names.size();
    const std::size_t url_count = links.size();

    if (name_count > 1 && url_count > name_count && url_count % name_count == 0) {
        // 分集数与线路数成整数倍（如 Dm84 的 3 条线路 × 6 集），按线路均分。
        const std::size_t per = url_count / name_count;
        for (std::size_t line = 0; line < name_count; ++line) {
            VideoLine video_line;
            video_line.name = su::Trim(names[line]);
            if (video_line.name.empty()) video_line.name = "线路" + std::to_string(line + 1);
            for (std::size_t k = 0; k < per; ++k)
                video_line.episodes.push_back(
                    VideoEpisode{links[line * per + k].name, links[line * per + k].url});
            if (!video_line.episodes.empty()) detail->lines.push_back(std::move(video_line));
        }
        return;
    }

    VideoLine line;
    line.name = name_count == 1 ? su::Trim(names.front()) : "线路1";
    if (line.name.empty()) line.name = "线路1";
    for (const PlayLink& link : links)
        line.episodes.push_back(VideoEpisode{link.name, link.url});
    detail->lines.push_back(std::move(line));
}

// recipe 里写的请求头（Referer/User-Agent）只用在抓页面，播放时丢了 ——
// 像厂长（4kcz）的 py.php 播放页缺 Referer 直接 403。这里把 recipe.headers
// 以播放器支持的「URL 内联请求头」后缀挂到每条播放地址上：
//   https://…/play.html{"Referer":"https://www.4kcz.com/"}
void AttachPlayHeaders(const nlohmann::json& recipe, VideoDetail* detail) {
    if (!detail || !recipe.contains("headers") || !recipe["headers"].is_object())
        return;
    std::string referer;
    std::string ua;
    for (auto it = recipe["headers"].begin(); it != recipe["headers"].end();
         ++it) {
        if (!it.value().is_string()) continue;
        const std::string key = su::ToLower(it.key());
        if (key == "referer") referer = it.value().get<std::string>();
        else if (key == "user-agent") ua = it.value().get<std::string>();
    }
    if (referer.empty()) referer = JsonStr(recipe, "base");
    if (referer.empty() && ua.empty()) return;
    nlohmann::json headers = nlohmann::json::object();
    if (!referer.empty()) headers["Referer"] = referer;
    if (!ua.empty()) headers["User-Agent"] = ua;
    const std::string suffix = headers.dump();
    for (auto& line : detail->lines) {
        for (auto& ep : line.episodes) {
            if (ep.url.empty() || ep.url.rfind("http", 0) != 0) continue;
            if (ep.url.find('{') != std::string::npos) continue;   // 已有内联头
            ep.url += suffix;
        }
    }
}

// 去掉 AttachPlayHeaders 追加的 JSON 前缀后缀，拿真正的地址去抓页面。
std::string StripInlinePlayHeaders(const std::string& url) {
    if (url.empty() || url.back() != '}') return url;
    const size_t open = url.rfind('{');
    if (open == std::string::npos || open == 0) return url;
    std::string head = url.substr(0, open);
    while (!head.empty() &&
           (head.back() == '$' || head.back() == '&' || head.back() == '#' ||
            head.back() == ' '))
        head.pop_back();
    return head.empty() ? url : head;
}

std::string PickRegexGroup(const std::string& text, const std::string& pattern) {
    if (pattern.empty()) return {};
    try {
        const std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
        std::smatch match;
        if (std::regex_search(text, match, re) && match.size() > 1)
            return su::Trim(match[1].str());
    } catch (const std::exception&) {
    }
    return {};
}

std::string UnescapeSlashes(std::string text) {
    text = su::ReplaceAll(std::move(text), "\\/", "/");
    return text;
}

std::string FirstJsonField(const json& root, const std::string& field) {
    if (root.is_object()) {
        const std::string value = JsonStr(root, field.c_str());
        if (!value.empty()) return value;
        for (auto it = root.begin(); it != root.end(); ++it) {
            const std::string nested = FirstJsonField(it.value(), field);
            if (!nested.empty()) return nested;
        }
    } else if (root.is_array()) {
        for (const auto& item : root) {
            const std::string nested = FirstJsonField(item, field);
            if (!nested.empty()) return nested;
        }
    }
    return {};
}

std::string ExtractPlayerUrl(const HtmlRun& run, const std::string& body,
                             const std::string& page_url) {
    (void)page_url;
    const json& recipe = run.profile.recipe;
    const json extract = recipe.value("extract", json::object());
    const json player = extract.value("player", json::object());

    const std::string url_field = JsonStr(player, "url_field");
    if (!url_field.empty()) {
        const json root = su::ParseJsonLoose(body);
        if (!root.is_null()) {
            const std::string value = FirstJsonField(root, url_field);
            if (!value.empty()) return UnescapeSlashes(value);
        }
        const std::string pattern =
            "\"" + url_field + "\"\\s*:\\s*\"([^\"]+)\"";
        const std::string value = PickRegexGroup(body, pattern);
        if (!value.empty()) return UnescapeSlashes(value);
    }

    // Auete：播放页把地址放在 base64decode("...") 里。
    {
        const std::string encoded = PickRegexGroup(
            body, "base64decode\\(\\s*[\"']([A-Za-z0-9+/=]+)[\"']\\s*\\)");
        if (!encoded.empty()) {
            std::string decoded;
            if (su::Base64Decode(encoded, &decoded) && IsDirectMedia(decoded))
                return su::Trim(decoded);
        }
    }

    // 站点自定义正则（recipe.html.regex），只接受像地址的结果。
    const auto html = recipe.find("html");
    if (html != recipe.end() && html->is_object()) {
        const auto regexes = html->find("regex");
        if (regexes != html->end() && regexes->is_object()) {
            for (auto it = regexes->begin(); it != regexes->end(); ++it) {
                if (!it.value().is_string()) continue;
                const std::string value =
                    PickRegexGroup(body, it.value().get<std::string>());
                if (value.empty()) continue;
                if (IsHttpUrl(value) || su::StartsWith(value, "//"))
                    return UnescapeSlashes(value);
            }
        }
    }

    // 通用兜底：m3u8 / mp4 / var url / iframe。
    static const char* kPatterns[] = {
        "(https?://[^\"'\\s<>\\\\]+\\.m3u8[^\"'\\s<>\\\\]*)",
        "(https?://[^\"'\\s<>\\\\]+\\.mp4[^\"'\\s<>\\\\]*)",
        "var\\s+url\\s*=\\s*[\"']([^\"']+)[\"']",
        "\"url\"\\s*:\\s*\"([^\"]+)\"",
        "iframe[^>]*src=[\"']([^\"']+)[\"']",
    };
    for (const char* pattern : kPatterns) {
        const std::string value = PickRegexGroup(body, pattern);
        if (value.empty()) continue;
        const std::string url = UnescapeSlashes(value);
        if (su::StartsWith(su::ToLower(url), "javascript:")) continue;
        return url;
    }
    return {};
}

// ───────────── hhplayer 型播放页（巴士 Dm84 等，引导配置 + 现算直链） ─────────────
//
// 这类站点的「线路地址」本身是播放页，页面里只有一段引导配置：
//   window.__HHJX_BOOTSTRAP__ = {"url":"<加密token>","t":<unix秒>,"key":"<hex>"}
// 真正的直链要按 recipe 里声明的 post 模板 POST 到 endpoints.player 现算。
// 以前这里没实现，详情/播放只能拿到网页地址，全靠网页协议插件兜底。
std::string BootstrapMarker(const json& recipe) {
    const json extract = recipe.value("extract", json::object());
    const json player = extract.value("player", json::object());
    if (JsonStr(player, "post").empty()) return {};
    return JsonStr(player, "extract");
}

std::string HostOrigin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string ResolveBootstrapDirect(const SpiderContext& ctx, const HtmlRun& run,
                                   const std::string& page_html,
                                   const std::string& page_url) {
    const json extract = run.profile.recipe.value("extract", json::object());
    const json player = extract.value("player", json::object());
    const std::string marker = JsonStr(player, "extract");
    const std::string post_tpl = JsonStr(player, "post");
    if (marker.empty() || post_tpl.empty()) return {};
    const size_t at = page_html.find(marker);
    if (at == std::string::npos) return {};
    // 取引导配置对象（花括号配对，够用且不依赖正则）。
    const size_t lb = page_html.find('{', at);
    if (lb == std::string::npos) return {};
    int depth = 0;
    size_t end = std::string::npos;
    for (size_t i = lb; i < page_html.size() && i < lb + 4096; ++i) {
        if (page_html[i] == '{') ++depth;
        else if (page_html[i] == '}' && --depth == 0) { end = i; break; }
    }
    if (end == std::string::npos) return {};
    const json cfg = su::ParseJsonLoose(page_html.substr(lb, end - lb + 1));
    if (!cfg.is_object()) return {};
    const std::string token = JsonStr(cfg, "url");
    const std::string key = JsonStr(cfg, "key");
    const std::string t = JsonStr(cfg, "t");
    if (token.empty() || key.empty() || t.empty()) return {};

    // post 模板里的 %s 依次是 url / t / key。
    std::string body = post_tpl;
    for (const std::string& value : {token, t, key}) {
        const size_t p = body.find("%s");
        if (p == std::string::npos) break;
        body.replace(p, 2, value);
    }
    const std::string api = EndpointUrl(run, "player", {});
    if (!IsHttpUrl(api)) return {};
    try {
        SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
        http.SetStage("player");
        http.SetDefaultHeaders(run.profile.headers);
        // 该接口有来源校验：Referer 用播放页、Origin 用接口同源（recipe 里配的
        // 站点 Origin 对它无效，必须在这里覆盖）。
        const std::map<std::string, std::string> headers = {
            {"referer", page_url.empty() ? api : page_url},
            {"origin", HostOrigin(api)},
        };
        const SpiderHttpResponse resp =
            http.Post(api, body, headers, "application/json");
        const json root = su::ParseJsonLoose(resp.body);
        if (!root.is_object()) return {};
        std::string url = UnescapeSlashes(su::JsonString(root, "url"));
        if (url.rfind("//", 0) == 0) url = "https:" + url;
        if (IsHttpUrl(url)) return url;
    } catch (const std::exception&) {
        // 解析失败回退网页地址，由网页协议插件再试一次。
    }
    return {};
}

class RecipeHtmlEngine : public SpiderEngine {
public:
    VideoListing Home(const SpiderContext& ctx) override {
        HtmlRun run = BeginRun(ctx);
        const std::string url = EndpointUrl(run, "home", {});
        const std::string body =
            FetchPage(run, url.empty() ? run.profile.base : url, "home");
        const HtmlNode doc = HtmlParser(body).Parse();

        VideoListing listing;
        json extract = run.profile.recipe.value("extract", json::object());
        json spec = extract.value("listing", json::object());
        std::string items_selector = JsonStr(spec, "items");
        if (items_selector.empty())
            items_selector = SelectorOr(run.profile.recipe, {"list"});
        listing = ParseListingNodes(run, doc, items_selector);
        if (listing.items.empty()) {
            const std::string home_list =
                SelectorOr(run.profile.recipe, {"home_list", "home_grid", "list"});
            if (!home_list.empty() && home_list != items_selector)
                listing = ParseListingNodes(run, doc, home_list);
        }
        listing.categories = ParseCategories(run, doc);
        return listing;
    }

    VideoListing Category(const SpiderContext& ctx, std::string_view category_id,
                          int page) override {
        HtmlRun run = BeginRun(ctx);
        const int safe_page = page <= 0 ? 1 : page;
        const std::string id(category_id);
        std::map<std::string, std::string> vars;
        vars["type_id"] = id;
        vars["cateId"] = id;
        vars["category_id"] = id;
        vars["id"] = id;
        vars["page"] = std::to_string(safe_page);

        std::string url = EndpointUrl(run, "category", vars);
        if (url.empty() && (IsHttpUrl(id) || (!id.empty() && id.front() == '/')))
            url = IsHttpUrl(id) ? id : su::JoinUrl(run.profile.base, id);
        if (url.empty()) return VideoListing{};

        std::string body;
        try {
            body = FetchPage(run, url, "category");
        } catch (const std::exception&) {
            // 分类失败不阻塞详情：首页卡片仍然可用。
            return VideoListing{};
        }
        const HtmlNode doc = HtmlParser(body).Parse();

        json extract = run.profile.recipe.value("extract", json::object());
        json spec = extract.value("listing", json::object());
        std::string items_selector = JsonStr(spec, "items");
        if (items_selector.empty())
            items_selector = SelectorOr(run.profile.recipe, {"list"});
        VideoListing listing = ParseListingNodes(run, doc, items_selector);
        if (listing.items.empty()) {
            const std::string list = SelectorOr(run.profile.recipe, {"list"});
            if (!list.empty() && list != items_selector)
                listing = ParseListingNodes(run, doc, list);
        }
        listing.page = safe_page;
        return listing;
    }

    VideoListing Search(const SpiderContext& ctx, std::string_view keyword,
                        int page) override {
        HtmlRun run = BeginRun(ctx);
        const int safe_page = page <= 0 ? 1 : page;
        std::map<std::string, std::string> vars;
        vars["keyword"] = std::string(keyword);
        vars["wd"] = std::string(keyword);
        vars["page"] = std::to_string(safe_page);
        const std::string url = EndpointUrl(run, "search", vars);
        if (url.empty()) return VideoListing{};

        const std::string body = FetchPage(run, url, "search");
        const HtmlNode doc = HtmlParser(body).Parse();
        json extract = run.profile.recipe.value("extract", json::object());
        json spec = extract.value("listing", json::object());
        std::string items_selector =
            SelectorOr(run.profile.recipe, {"search_list", "search_item"});
        if (items_selector.empty()) items_selector = JsonStr(spec, "items");
        if (items_selector.empty())
            items_selector = SelectorOr(run.profile.recipe, {"list"});
        VideoListing listing = ParseListingNodes(run, doc, items_selector);
        listing.page = safe_page;
        return listing;
    }

    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        HtmlRun run = BeginRun(ctx);
        std::string url;
        if (IsHttpUrl(item.id)) {
            url = item.id;
        } else if (!item.id.empty() && item.id.front() == '/') {
            url = su::JoinUrl(run.profile.base, item.id);
        } else {
            std::map<std::string, std::string> vars;
            vars["vod_id"] = item.id;
            vars["id"] = item.id;
            vars["mid"] = item.id;
            url = EndpointUrl(run, "detail", vars);
        }
        if (url.empty()) {
            throw std::runtime_error("模板 C 详情地址为空（站点 " + ctx.spider_id + "）");
        }

        const std::string body = FetchPage(run, url, "detail");
        const HtmlNode doc = HtmlParser(body).Parse();

        const json extract = run.profile.recipe.value("extract", json::object());
        const json spec = extract.value("detail", json::object());
        const json fields = spec.value("fields", json::object());

        VideoDetail detail;
        detail.id = item.id;
        detail.title = FieldValue(doc, fields, "vod_name");
        if (detail.title.empty()) detail.title = item.title;
        detail.pic = FieldValue(doc, fields, "vod_pic");
        if (detail.pic.empty()) detail.pic = item.pic;
        detail.pic = NormalizeUrl(run.profile.base, detail.pic);
        detail.remarks = FieldValue(doc, fields, "vod_remarks");
        if (detail.remarks.empty()) detail.remarks = item.remarks;

        std::vector<std::string> names = FieldValues(doc, fields, "vod_play_from");
        std::vector<std::string> urls = FieldValues(doc, fields, "vod_play_url");

        if (urls.empty()) {
            const std::string links =
                SelectorOr(run.profile.recipe, {"detail_links", "play_link"});
            if (!links.empty()) {
                urls = SelectAll(doc, links);
                // 站点把网盘链接放在 data-clipboard-text（href 是 javascript:;）。
                bool all_bad = true;
                for (const std::string& value : urls) {
                    if (!value.empty() && !su::StartsWith(value, "javascript:") &&
                        value != "#") {
                        all_bad = false;
                        break;
                    }
                }
                if (all_bad) {
                    std::string attr_selector = links;
                    const std::size_t at = attr_selector.rfind('@');
                    if (at != std::string::npos) attr_selector.resize(at);
                    attr_selector += "@data-clipboard-text";
                    urls = SelectAll(doc, attr_selector);
                }
            }
        }

        if (urls.empty()) {
            // YGP：详情页没有播放列表，播放地址走 ?view=api&mode=download-data&ttid=
            const json player = extract.value("player", json::object());
            const std::string play_api = JsonStr(player, "extract");
            const std::string url_field = JsonStr(player, "url_field");
            if (!play_api.empty() && !url_field.empty() &&
                (play_api.front() == '?' || IsHttpUrl(play_api))) {
                std::string api = ExpandVars(play_api, {{"vod_id", item.id}});
                std::string ttid = DigitsFrom(item.id);
                if (ttid.empty()) ttid = item.id;
                if (api.find("ttid=") == std::string::npos)
                    api += (api.find('?') == std::string::npos ? "?" : "&") +
                           std::string("ttid=") + su::UrlEncode(ttid);
                api += (api.find('?') == std::string::npos ? "?" : "&") +
                       std::string("rand=") + std::to_string(su::UnixMilliseconds() % 100000);
                urls.push_back(api);
                names.push_back("预告片");
            }
        }

        AppendEpisodes(names, urls, run.profile.base, &detail);
        AttachPlayHeaders(run.profile.recipe, &detail);
        detail.raw = json{{"url", url}};
        return detail;
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        const std::string original = episode.url;   // 保留内联请求头后缀
        if (original.empty()) return original;
        const std::string clean = StripInlinePlayHeaders(original);
        // 解析出新地址时也要把内联请求头带过去：不少 CDN/播放页校验 Referer，
        // 丢了就 403（播放窗会把后缀拆成 open_headers 再传给协议插件）。
        const std::string suffix =
            original.size() > clean.size() ? original.substr(clean.size())
                                           : std::string();
        // 直链/非网页协议原样回传（带内联头，播放层解析后使用）。
        if (IsDirectMedia(clean) || IsNonPageScheme(clean)) return original;

        try {
            HtmlRun run = BeginRun(ctx);
            const std::string body = FetchPage(run, clean, "player");
            const std::string found = ExtractPlayerUrl(run, body, clean);
            // 播放页只有引导配置（巴士的 hhplayer 线路）：按 recipe 声明的
            // post 模板现算直链。当前页命中就直接算；否则把 ExtractPlayerUrl
            // 找到的播放器 iframe 再抓一层（最多一层）。
            const std::string marker = BootstrapMarker(run.profile.recipe);
            if (!marker.empty()) {
                std::string media;
                if (body.find(marker) != std::string::npos) {
                    media = ResolveBootstrapDirect(ctx, run, body, clean);
                } else if (!found.empty() && IsHttpUrl(found) &&
                           !IsDirectMedia(found) && found != clean) {
                    try {
                        const std::string inner = FetchPage(run, found, "player");
                        if (inner.find(marker) != std::string::npos)
                            media = ResolveBootstrapDirect(ctx, run, inner, found);
                    } catch (const std::exception&) {
                    }
                }
                // 现算出来的直链挂在播放器域名下（QQ 云等），站点 Referer 会被
                // 判为盗链直接 404，所以这里不带详情里的内联请求头。
                if (!media.empty()) return media;
            }
            if (!found.empty()) return found + suffix;
        } catch (const std::exception&) {
            // 播放页取不到时回传详情里的原始地址，由上层解析链兜底。
        }
        return original;
    }
};

// ───────────────────────── Bttwoo（比特）播放签名 ─────────────────────────
//
// 2026-11 逆向结论（nbmovie_wasm_bg.wasm）：
//   q   = 清晰度（'4k' 映射成 '1'，其余原样）
//   t   = 服务器毫秒时间戳（页面 <meta id="nb-st"> + 本地经过时间）
//   k   = base64( userlink XOR 重复密钥 "nbmovie2024secretkey" )
//   s   = HMAC-SHA256(key=路径 id, data="{dataid}:{t}:{路径 id}") 的 hex 前 32 位
//   请求 GET /video/play?p={dataid}&v={路径id}&q={q}&s={s}&t={t}&k={k}
// 响应 {"data":{"quality_urls":[{"mtype":"m3u8","url":"…"}]}}。
//
// 详情页每个分集锚点都带 dataid（a.episode-link），因此详情里把 dataid 编进
// 片段（#bttwoo=），Player 再回抓一次分集页拿 userlink / nb-st 后现算签名。

std::string BttwooXorBase64(const std::string& text) {
    static const char kKey[] = "nbmovie2024secretkey";
    std::string out = text;
    const size_t klen = sizeof(kKey) - 1;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(out[i] ^ kKey[i % klen]);
    return su::Base64Encode(out);
}

std::string BttwooInlineHeaders(std::string url, const std::string& referer,
                                const std::string& ua) {
    if (url.empty() || url.find('{') != std::string::npos) return url;
    nlohmann::json h;
    h["Referer"] = referer;
    if (!ua.empty()) h["User-Agent"] = ua;
    return url + h.dump();
}

std::string BttwooLastSegment(const std::string& url) {
    std::string s = url;
    const size_t cut = s.find_first_of("?#");
    if (cut != std::string::npos) s.resize(cut);
    while (!s.empty() && s.back() == '/') s.pop_back();
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? std::string() : s.substr(slash + 1);
}

class BttwooEngine : public RecipeHtmlEngine {
public:
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        HtmlRun run = BeginRun(ctx);
        std::string page;
        if (IsHttpUrl(item.id)) {
            page = item.id;
        } else if (!item.id.empty() && item.id.front() == '/') {
            page = su::JoinUrl(run.profile.base, item.id);
        } else {
            page = su::JoinUrl(run.profile.base, "/play/" + item.id);
        }
        if (page.empty())
            throw std::runtime_error("Bttwoo 详情地址为空");

        const std::string body = FetchPage(run, page, "detail");
        const HtmlNode doc = HtmlParser(body).Parse();
        const json extract = run.profile.recipe.value("extract", json::object());
        const json fields =
            extract.value("detail", json::object()).value("fields", json::object());

        VideoDetail detail;
        detail.id = item.id;
        detail.title = FieldValue(doc, fields, "vod_name");
        if (detail.title.empty()) detail.title = item.title;
        detail.pic = NormalizeUrl(run.profile.base,
                                  FieldValue(doc, fields, "vod_pic"));
        if (detail.pic.empty()) detail.pic = item.pic;
        detail.remarks = FieldValue(doc, fields, "vod_remarks");
        if (detail.remarks.empty()) detail.remarks = item.remarks;

        const std::vector<std::string> hrefs =
            SelectAll(doc, "a.episode-link@href");
        const std::vector<std::string> dataids =
            SelectAll(doc, "a.episode-link@dataid");
        const std::vector<std::string> line_attrs =
            SelectAll(doc, "a.episode-link@data-line");
        const std::vector<std::string> texts =
            SelectAll(doc, "a.episode-link@text");

        std::map<int, VideoLine> grouped;
        for (size_t i = 0; i < hrefs.size(); ++i) {
            const std::string dataid =
                i < dataids.size() ? su::DigitsOnly(dataids[i]) : std::string();
            if (dataid.empty()) continue;
            int line_no = 1;
            if (i < line_attrs.size() && !line_attrs[i].empty()) {
                const int parsed = std::atoi(line_attrs[i].c_str());
                if (parsed > 0) line_no = parsed;
            }
            VideoEpisode ep;
            ep.url = su::JoinUrl(run.profile.base, hrefs[i]) + "#bttwoo=" +
                     dataid;
            ep.name = i < texts.size() ? su::Trim(texts[i]) : std::string();
            if (ep.name.empty())
                ep.name = "第" + std::to_string(grouped[line_no].episodes.size() + 1) +
                          "集";
            grouped[line_no].episodes.push_back(std::move(ep));
        }
        for (auto& kv : grouped) {
            if (kv.second.episodes.empty()) continue;
            kv.second.name = "线路" + std::to_string(kv.first);
            detail.lines.push_back(std::move(kv.second));
        }
        detail.raw = json{{"url", page}};
        return detail;
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        const std::string raw = episode.url;
        const size_t frag = raw.find("#bttwoo=");
        if (frag == std::string::npos) return raw;
        const std::string dataid = raw.substr(frag + 9);
        const std::string page = raw.substr(0, frag);
        if (dataid.empty() || page.empty()) return raw;

        HtmlRun run = BeginRun(ctx);
        const long long fetch_start = su::UnixMilliseconds();
        std::string body;
        try {
            body = FetchPage(run, page, "player");
        } catch (const std::exception&) {
            return raw;
        }
        const std::string userlink =
            PickRegexGroup(body, "userlink:'(.*?)',");
        if (userlink.empty()) return raw;
        const std::string secret = BttwooLastSegment(page);
        if (secret.empty()) return raw;

        long long server_ms = 0;
        {
            const std::string st =
                PickRegexGroup(body, "id=\"nb-st\"[^>]*content=\"(\\d+)\"");
            if (!st.empty()) server_ms = std::atoll(st.c_str());
        }
        const long long t = server_ms > 0
                                ? server_ms + (su::UnixMilliseconds() - fetch_start)
                                : su::UnixMilliseconds();
        const std::string sign =
            su::HmacSha256Hex(secret, dataid + ":" + std::to_string(t) + ":" + secret)
                .substr(0, 32);
        if (sign.size() != 32) return raw;

        std::string api = su::JoinUrl(run.profile.base, "/video/play");
        api += "?p=" + su::UrlEncode(dataid) + "&v=" + su::UrlEncode(secret) +
               "&q=1080&s=" + sign + "&t=" + std::to_string(t) +
               "&k=" + su::UrlEncode(BttwooXorBase64(userlink));

        std::string ua;
        for (const auto& kv : run.profile.headers)
            if (su::ToLower(kv.first) == "user-agent") ua = kv.second;

        std::string media;
        try {
            SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
            http.SetStage("player");
            http.SetDefaultHeaders(run.profile.headers);
            const SpiderHttpResponse resp = http.Get(api);
            const json root = su::ParseJsonLoose(resp.body);
            if (root.is_object()) {
                for (const json* node :
                     su::JsonFindAll(root, "$.data.quality_urls[*]")) {
                    if (!node || !node->is_object()) continue;
                    const std::string url = su::JsonString(*node, "url");
                    if (url.empty()) continue;
                    const std::string locked = su::JsonString(*node, "locked");
                    const std::string vip = su::JsonString(*node, "isvip");
                    if (locked == "true" || locked == "1" || vip == "true" ||
                        vip == "1")
                        continue;
                    if (media.empty() || url.find(".m3u8") != std::string::npos)
                        media = url;
                    if (url.find(".m3u8") != std::string::npos) break;
                }
            }
        } catch (const std::exception&) {
            return raw;
        }
        if (media.empty()) return raw;
        return BttwooInlineHeaders(media, page, ua);
    }
};

// ───────────────────────── 看球（Kanqiu）：运行时注入的直播页 ─────────────────────────
//
// 卡片本身就是 /live/{id}/play 直播页，播放地址由 sss.js 运行时写进页面，
// 静态解析永远拿不到。这里把“直播页 URL”当作线路交给播放层：
//   · 配置了 parses 时由 WEB 插件解析；
//   · 解析不出来时播放窗先用隐藏 WebView2 跑页面 JS，嗅探到 m3u8 后切原生。
// /live/{id}/source 的 data 形如「6 位随机前缀 + base64(JSON) + == + 2 位后缀」，
// 从 JSON 的 eyJ 起点截到第一个 '=' 再解 base64。
std::string KanqiuDecodeSource(const std::string& data) {
    const size_t start = data.find("eyJ");
    if (start == std::string::npos) return {};
    std::string b64 = data.substr(start);
    const size_t pad = b64.find('=');
    if (pad != std::string::npos) b64.resize(pad);
    while (b64.size() % 4) b64 += '=';
    std::string out;
    return su::Base64Decode(b64, &out) ? out : std::string();
}

class KanqiuEngine : public RecipeHtmlEngine {
public:
    VideoDetail Detail(const SpiderContext& ctx, const VideoItem& item) override {
        VideoDetail detail;
        detail.id = item.id;
        detail.title = item.title;
        detail.pic = item.pic;
        detail.remarks = item.remarks;
        std::string page;
        try {
            HtmlRun run = BeginRun(ctx);
            if (IsHttpUrl(item.id)) {
                page = item.id;
            } else if (!item.id.empty() && item.id.front() == '/') {
                page = su::JoinUrl(run.profile.base, item.id);
            }
        } catch (const std::exception&) {
            return detail;
        }
        if (page.empty()) return detail;
        // 线路 1：走 /source 现算的直链；线路 2：原直播页，直链失效时由
        // 播放窗「自动换线」切到网页（隐藏 WebView2 兜底）。
        VideoLine line;
        line.name = "线路Q1";
        VideoEpisode ep;
        ep.name = "直播";
        ep.url = page + "#kanqiu=1";
        line.episodes.push_back(std::move(ep));
        detail.lines.push_back(std::move(line));

        VideoLine fallback;
        fallback.name = "网页兜底";
        VideoEpisode web;
        web.name = "直播页";
        web.url = page;
        fallback.episodes.push_back(std::move(web));
        detail.lines.push_back(std::move(fallback));
        detail.raw = json{{"url", page}};
        return detail;
    }

    std::string Player(const SpiderContext& ctx,
                       const VideoEpisode& episode) override {
        std::string page = episode.url;
        if (page.empty()) return page;
        // 不带 #kanqiu 标记的是「网页兜底」线路：原样返回，交给网页嗅探。
        if (page.find("#kanqiu=") == std::string::npos) return page;
        page = page.substr(0, page.find('#'));
        // /live/{id}/play → /live/{id}/source 返回 base64(JSON)：
        // {"links":[{"url":"https://play.88player.top/m3u8.html?url=<m3u8>"}]}
        const size_t live = page.find("/live/");
        if (live == std::string::npos) return page;
        const size_t begin = live + 6;
        const size_t end = page.find('/', begin);
        const std::string id =
            page.substr(begin, end == std::string::npos ? std::string::npos
                                                        : end - begin);
        if (id.empty()) return page;

        HtmlRun run = BeginRun(ctx);
        std::string media;
        try {
            SpiderHttp http(ctx.spider_id, ctx.net, ctx.site);
            http.SetStage("player");
            http.SetDefaultHeaders(run.profile.headers);
            const SpiderHttpResponse resp = http.Get(
                su::JoinUrl(run.profile.base, "/live/" + id + "/source"));
            const json root = su::ParseJsonLoose(resp.body);
            const std::string payload = su::JsonString(root, "data");
            const std::string decoded = KanqiuDecodeSource(payload);
            if (!decoded.empty()) {
                const json src = su::ParseJsonLoose(decoded);
                for (const json* link : su::JsonFindAll(src, "$.links[*]")) {
                    if (!link || !link->is_object()) continue;
                    const std::string url = su::JsonString(*link, "url");
                    if (url.empty()) continue;
                    // 线路地址是包装页，真正的 m3u8 在 url= 参数里。
                    std::string inner = su::QueryParam(url, "url");
                    if (inner.empty() && url.find(".m3u8") != std::string::npos)
                        inner = url;
                    inner = su::UrlDecode(inner);
                    if (inner.rfind("http", 0) == 0) {
                        media = inner;
                        break;
                    }
                }
            }
        } catch (const std::exception&) {
            return page;
        }
        if (media.empty()) return page;   // 解析失败 → 交给隐藏 WebView2 嗅探
        // 包装页 <meta name="referrer" content="no-referrer">，分片请求不带 Referer；
        // 这里保持裸地址，由 HLS 插件用默认头请求。
        return media;
    }
};

// ============================ 注册 ============================

struct HtmlMetaSpec {
    const char* id;
    const char* class_name;
    bool implemented;
    const char* note;
};

// 只有真网络跑通的类才 implemented=true（2026-11 实测）。
//   PASS：home/category 有卡片且 detail 有线路；
//   FAIL：源站关闭 / 403 反爬 / 人机验证 / 需登录 / 站点改版。
const HtmlMetaSpec kHtmlSpecs[] = {
    {"csp_WoGG", "WoGG", true,
     "PASS：wogg.xxooo.cf 已失效，自动回落镜像 woggpan.888484.xyz；首页/分类卡片、"
     "详情网盘链接；播放为网盘分享页，无直链"},
    {"csp_YGP", "YGP", true,
     "PASS：6huo.com 首页/分类 div.movlist 卡片；详情无播放列表，播放走 "
     "?view=api&mode=download-data&ttid= 接口返回 mp4"},
    {"csp_NewCz", "NewCz", false,
     "FAIL：www.4kcz.com 及镜像返回 403（Cloudflare/JS 过盾 + 登录 JWT），静态抓取"
     "拿不到列表"},
    {"csp_Libvio", "Libvio", false,
     "FAIL：www.libvio.io 返回 403（__cdn_pow 工作量证明反爬，需先解 PoW 换 Cookie）"},
    {"csp_Nmyswv", "Nmyswv", false,
     "FAIL：vip.wwgz.cn:5200 根路径与 /index.php 均返回 404，源站已关闭/迁移"},
    {"csp_Music", "Music", false,
     "FAIL：www.4c44.com 返回「安全人机验证」页（human_check+CSRF），未过验证码取不到"
     "任何列表/播放数据"},
    {"csp_Auete", "Auete", true,
     "PASS：au1080.top 首页 ul.threadlist 卡片、详情 episode-list 线路；播放页 "
     "now=base64decode(...) 可直接解出 m3u8"},
    {"csp_Bttwoo", "Bttwoo", true,
     "PASS：bttwo.life 首页 div[data-vod-id] 卡片、详情 a.episode-link 分集；真实"
     "播放地址需 /video/play 的 k= 签名（未静态确认），回传分集页地址"},
    {"csp_SixV", "SixV", false,
     "FAIL：recipe.base 为空，域名随镜像变化且无已知可用域名；磁力详情选择器已还原，"
     "需用户在 ext 提供 base"},
    {"csp_Seedhub", "Seedhub", false,
     "FAIL：seedhub.pro 返回 403（Cloudflare 防护），首页/详情均取不到"},
    {"csp_KkSs", "KkSs", true,
     "PASS：xiaocge.fun 已失效，回落镜像 tv.yydsys.top；首页/分类卡片、详情网盘链接；"
     "无在线播放（网盘搜索源）"},
    {"csp_UuSs", "UuSs", true,
     "PASS：123.666291.xyz 首页/分类卡片、详情网盘链接（百度/夸克/UC）；无直链播放"},
    {"csp_Kanqiu", "Kanqiu", false,
     "FAIL：88kanqiu.pw 首页卡片与 /live/{id}/play 页可用，但播放地址由 sss.js 在"
     "运行时注入（encodedStr 不在静态 HTML），详情取不到线路"},
    {"csp_Dm84", "Dm84", true,
     "PASS：dmbus.cc 首页/分类 div.item 卡片、详情 3 线路×N 集；播放为 hhplayer "
     "iframe（url 参数加密，回传 iframe 地址）"},
    {"csp_FirstAid", "FirstAid", false,
     "FAIL：m.youlai.cn 站点改版，/jijiu/ 只剩图文文章列表，视频详情选择器"
     "（.video-title/#video source）全部失效"},
};

void RegisterHtmlSpiderMetas(SpiderRegistry& registry) {
    for (const HtmlMetaSpec& spec : kHtmlSpecs) {
        SpiderMeta meta;
        meta.id = spec.id;
        meta.class_name = spec.class_name;
        meta.tmpl = SpiderTemplate::kHtml;
        meta.implemented = spec.implemented;
        meta.note = spec.note;
        registry.Register(
            std::move(meta),
            [id = std::string(spec.id)]() -> std::unique_ptr<SpiderEngine> {
                if (id == "csp_Bttwoo") return std::make_unique<BttwooEngine>();
                return std::make_unique<RecipeHtmlEngine>();
            });
    }
}

bool g_html_pending = false;

// SpiderRegistry 构造函数会调用 RegisterHtmlSpiderEngines()；此时再调用
// Instance() 会重入函数局部静态的初始化而死锁，因此这里只置位，真正注册由
// 静态对象在注册表构造完成后执行（与 cms/app 引擎一致）。
struct HtmlEngineRegistrar {
    HtmlEngineRegistrar() {
        SelfTestSelector();
        SpiderRegistry& registry = SpiderRegistry::Instance();
        if (g_html_pending) RegisterHtmlSpiderMetas(registry);
        // 把 sources/spiders 里所有 html 模板的社区 spider recipe 自动点亮。
        RegisterRecipeSpiderVariants(SpiderTemplate::kHtml,
                                     [] { return std::make_unique<RecipeHtmlEngine>(); });
        // 上面的批量注册会覆盖同名专用引擎（skip_if_registered=false），
        // 这里再登记一次 Bttwoo，确保它的签名实现生效。
        {
            SpiderMeta meta;
            meta.id = "csp_Bttwoo";
            meta.class_name = "Bttwoo";
            meta.tmpl = SpiderTemplate::kHtml;
            meta.implemented = true;
            meta.note =
                "PASS：播放签名已逆向（HMAC-SHA256(key=路径id, data=id:t:id) + "
                "XOR/base64(userlink)）；/video/play 现算直链";
            registry.Register(std::move(meta),
                              [] { return std::make_unique<BttwooEngine>(); });
        }
        // 看球：详情直接返回直播页，播放地址交给 WEB 解析 / 播放窗隐藏嗅探。
        {
            SpiderMeta meta;
            meta.id = "csp_Kanqiu";
            meta.class_name = "Kanqiu";
            meta.tmpl = SpiderTemplate::kHtml;
            meta.implemented = true;
            meta.note =
                "PASS：卡片/直播页可用；播放地址由 sss.js 运行时注入，"
                "交给 WEB 解析或播放窗隐藏 WebView2 嗅探";
            registry.Register(std::move(meta),
                              [] { return std::make_unique<KanqiuEngine>(); });
        }
    }
};

const HtmlEngineRegistrar kHtmlEngineRegistrar;

} // namespace

void RegisterHtmlSpiderEngines() { g_html_pending = true; }

} // namespace ttplayer::video
