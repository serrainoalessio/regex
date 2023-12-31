#include <iostream>
#include <iomanip>
#include <fstream>
#include <stack>
#include <array>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>
#include <string_view>
#include <cassert>
#include <sstream>
#include <memory>
#include <set>
#include <exception>
#include <functional>
#include <type_traits>

// Exception classes during the parsing of exceptions
class syntax_error : public std::exception {
public:
    explicit syntax_error(const std::string& message) : message_(message) {}

    // Override the what() method to provide a description of the exception
    const char* what() const noexcept override {
        return message_.c_str();
    }

private:
    std::string message_;
};

class unbalanced_brackets : public std::exception {
public:
    explicit unbalanced_brackets(const std::string& message) : message_(message) {}

    // Override the what() method to provide a description of the exception
    const char* what() const noexcept override {
        return message_.c_str();
    }

private:
    std::string message_;
};


// Node class for the Abstract Syntax Tree
struct ASTNode {
    // A virtual method is required to implement polymorphic type
    virtual ~ASTNode() = default;  // Polymorphic type
    ASTNode *parent = nullptr;  // Actually not needed
    virtual size_t priority() const = 0;  // 0 = highest priotity

    template<typename T>
    bool isinstance() { return dynamic_cast<T*>(this); }  // If not nullptr
    virtual bool accept_epsilon() const = 0;  // True if node accepts epsilon-string
};

struct SingleChildNode : public ASTNode {
    template<typename T>
    SingleChildNode(T p_child) : child(std::move(p_child)) {
        this->child->parent = this;  // Note: child argument is a null pointer now
    }

    template<typename Arg>
    void set_node(Arg&& args) {
        this->child = std::move(args);
        this->child->parent = this;  // Sets the parent of the new child
    }

    std::unique_ptr<ASTNode> child;
    bool greedy = true;
};

struct MultiChildNode : public ASTNode {
    template<typename... Args>
    MultiChildNode(Args... init) {
        // Unfortunately the only way to initialize a vector of
        // only-movable objects is through successive calls to emplace_back
        ((init->parent = this), ...);  // This must be done BEFORE moving the pointer
        ((childs.emplace_back(std::move(init))), ...);
    }

    template<typename... Args>
    void append_node(Args&&... args) {
        childs.emplace_back(std::forward<Args>(args)...);
        childs.back()->parent = this;  // Sets the parent of the new child
    }

    template<typename... Args>
    void insert_node(size_t pos, Args&&... args) {
        childs.emplace(std::next(childs.begin(), pos), std::forward<Args>(args)...);
        (*std::next(childs.begin(), pos))->parent = this;  // Sets the parent of the new child
    }

    std::vector<std::unique_ptr<ASTNode>> childs;
};

struct BracketNode final : public SingleChildNode {
    using SingleChildNode::SingleChildNode;
    size_t priority() const override { return 0; }
    bool accept_epsilon() const override { return child->accept_epsilon(); }
    bool capture = false;
};
struct KleeneStarNode final : public SingleChildNode {
    using SingleChildNode::SingleChildNode;
    size_t priority() const override { return 1; }
    bool accept_epsilon() const override { return true; }
};
struct ConcatenationNode final : public MultiChildNode {
    using MultiChildNode::MultiChildNode;
    size_t priority() const override { return 2; }
    bool accept_epsilon() const override {
        for (auto&child:childs)
            if (!child->accept_epsilon())
                return false;
        return true;
    }
};
struct DisjunctionNode final : public MultiChildNode {
    using MultiChildNode::MultiChildNode;
    size_t priority() const override { return 3; }
    bool accept_epsilon() const override {
        for (auto&child:childs)
            if (child->accept_epsilon())
                return true;
        return false;
    }
};

// Basic matchers (used also by NFA transitions)
struct Matcher {
    virtual ~Matcher() = default;  // Polymorphic type
    virtual size_t length() const = 0;  // Characters consumed
    virtual bool match(const std::string_view& str) const = 0;
    virtual Matcher* clone() const = 0;
};

// Matches the empty node
struct EpsilonMatcher final : public ASTNode, public Matcher {
    EpsilonMatcher() {}
    size_t priority() const override { return 0; }
    size_t length() const override { return 0; }  // Does not read any character
    bool match(const std::string_view& str) const override { (void)str; return true; }  // Always matches
    EpsilonMatcher* clone() const override { return(new EpsilonMatcher(*this)); }
    bool accept_epsilon() const override { return true; }
};

struct CharacterMatcher final : public ASTNode, public Matcher {
    CharacterMatcher(char target) : cmatch(target) {}
    size_t length() const override { return 1; }  // Reads exactly one character
    size_t priority() const override { return 0; }
    CharacterMatcher* clone() const override { return(new CharacterMatcher(*this)); }
    char cmatch;
    // Never matches empty strings
    bool match(const std::string_view& str) const override { return !str.empty() && str[0] == cmatch; }
    bool accept_epsilon() const override { return false; }
};

struct UniversalMatcher final : public ASTNode, public Matcher {  // Matches everythong
    UniversalMatcher() {}
    size_t length() const override { return 1; }  // Reads exactly one character
    size_t priority() const override { return 0; }
    UniversalMatcher* clone() const override { return(new UniversalMatcher(*this)); }
    // Matches every non-empty string
    bool match(const std::string_view& str) const override { return !str.empty(); }
    bool accept_epsilon() const override { return false; }  // Accepts single characters
};

// === Extras ===
struct OneOrMoreNode final : public SingleChildNode {
    using SingleChildNode::SingleChildNode;
    size_t priority() const override { return 1; }
    bool accept_epsilon() const override { return child->accept_epsilon(); }
};  // The + operator
struct OneOrNoneNode final : public SingleChildNode {
    using SingleChildNode::SingleChildNode;
    size_t priority() const override { return 1; }
    bool accept_epsilon() const override { return true; }
};  // The ? operator
struct MultiplyNode final : public SingleChildNode {
    size_t priority() const override { return 1; }
    template<typename T>
    MultiplyNode(T p_child, size_t n_min = 0, size_t n_max = 0):
        SingleChildNode(std::move(p_child)), min(n_min), max(n_max) {}
    size_t min = 0, max = 0;
    bool unbounded = false;
    bool exact() const { return !unbounded && min == max; }  // True if exact number of matches
    bool accept_epsilon() const override { return min == 0 || child->accept_epsilon(); }
};  // The {m,n} operator

// Character classes (that is [...])
struct CharacterClassMatcher final : public ASTNode, public Matcher {
    CharacterClassMatcher() {}
    size_t priority() const override { return 0; }
    size_t length() const override { return 1; }  // Reads exactly one character
    bool accept_epsilon() const override { return false; }  // Accepts single characters
    CharacterClassMatcher* clone() const override { return(new CharacterClassMatcher(*this)); }
    bool invert = false;  // by default is false
    std::vector<std::pair<char, char>> intervals;

    bool _non_inverting_match(const std::string_view& str) const {
        for (auto&&interval:intervals)
            if (str[0] >= interval.first && str[0] <= interval.second)
                return true;
        return false;
    }

    bool match(const std::string_view& str) const override {
        return invert ^ _non_inverting_match(str);  // Xor acts like a controlled negation
    }


    bool operator==(const CharacterClassMatcher&other) const {
        // Assume both are normalized
        return (invert == other.invert) &&
               (intervals == other.intervals);
    }

    // Removes unuseful character classes
    void normalize() {
        if (intervals.empty()) return;

        for (auto& interval : intervals) {
            auto m = std::min(interval.first, interval.second);
            auto M = std::max(interval.first, interval.second);
            interval = std::make_pair(m, M);
        }

        // Sort intervals based on start values
        std::sort(intervals.begin(), intervals.end());

        int mergedIndex = 0; // Index of the last merged interval
        for (const auto& interval : intervals) {
            if (interval.first <= intervals[mergedIndex].second+1) {
                // Overlapping intervals, update the end value of the last merged interval
                intervals[mergedIndex].second = std::max(interval.second, intervals[mergedIndex].second);
            } else {
                // Non-overlapping interval, move it to the next position in the vector
                ++mergedIndex;
                intervals[mergedIndex] = interval;
            }
        }

        // Erase any remaining intervals after the merged index
        intervals.erase(intervals.begin() + mergedIndex + 1, intervals.end());
    }
    
    bool empty() const { return intervals.size() == 0; }  // Matches no character
    bool singlec() const {  // True if matches a single character
        return !invert && intervals.size() == 1 &&
               std::get<0>(intervals[0]) == std::get<1>(intervals[0]); 
    }
    char character() const { return intervals.size()?std::get<0>(intervals[0]):0; }
};

void optimizeAST(std::unique_ptr<ASTNode>& root);
struct AST {
    std::unique_ptr<ASTNode> root;
    bool anchorBegin = false;
    bool anchorEnd = false;
    void optimize() { optimizeAST(this->root); }
};

using namespace std; 
// Utility function to print the AST in a readable format
void _printAST(const std::unique_ptr<ASTNode>& root, int indent) {
    for (int i = 0; i < indent; ++i) {
        cout << "  ";
    }

    if (const CharacterMatcher* chr = dynamic_cast<const CharacterMatcher*>(root.get())) {
        cout << "CharacterMatcher: " << chr->cmatch << endl;
    } else if (const CharacterClassMatcher* cchr = dynamic_cast<const CharacterClassMatcher*>(root.get())) {
        cout << "CharacterClassMatcher: " << ((cchr->invert)?"invert":"") << endl;
        for (auto&&interval:cchr->intervals) {
            for (int i = 0; i < indent+1; ++i)
                cout << "  ";
            cout << std::get<0>(interval) << " " << std::get<1>(interval) << endl;
        }
    } else if (dynamic_cast<const UniversalMatcher*>(root.get())) {
        cout << "UniversalMatcher " << endl;
    } else if (dynamic_cast<const EpsilonMatcher*>(root.get())) {
        cout << "EpsilonMatcher" << endl;
    } else if (const ConcatenationNode* concat = dynamic_cast<const ConcatenationNode*>(root.get())) {
        cout << "Concatenation" << endl;
        for(const auto& child:concat->childs) {
            _printAST(child, indent + 1);
            assert(child->parent == root.get());
        }
    } else if (const DisjunctionNode* disj = dynamic_cast<const DisjunctionNode*>(root.get())) {
        cout << "Disjunction" << endl;
        for(const auto& child:disj->childs) {
            _printAST(child, indent + 1);
            assert(child->parent == root.get());
        }
    } else if (const KleeneStarNode* kleene = dynamic_cast<const KleeneStarNode*>(root.get())) {
        cout << "Kleene Star: " << ((kleene->greedy)?"greedy":"lazy") << endl;
        _printAST(kleene->child, indent + 1);
        assert(kleene->child->parent == root.get());
    } else if (const OneOrMoreNode* oneormore = dynamic_cast<const OneOrMoreNode*>(root.get())) {
        cout << "One or More: " << ((oneormore->greedy)?"greedy":"lazy") << endl;
        _printAST(oneormore->child, indent + 1);
        assert(oneormore->child->parent == root.get());
    } else if (const OneOrNoneNode* oneornone = dynamic_cast<const OneOrNoneNode*>(root.get())) {
        cout << "One or None: " << ((oneornone->greedy)?"greedy":"lazy") << endl;
        _printAST(oneornone->child, indent + 1);
        assert(oneornone->child->parent == root.get());
    } else if (const MultiplyNode* multiply = dynamic_cast<const MultiplyNode*>(root.get())) {
        cout << "multiply: " << multiply->min;
        if (!multiply->exact()) {
            if (!multiply->unbounded) cout << " " << multiply->max;
            else cout << "+";
        }
        cout << " " << ((multiply->greedy)?"greedy":"lazy") << endl;
        _printAST(multiply->child, indent + 1);
        assert(multiply->child->parent == root.get());
    } else if (const BracketNode* bracket = dynamic_cast<const BracketNode*>(root.get())) {
        cout << "Parenthesis" << ((bracket->capture)?" capturing":"") << endl;
        _printAST(bracket->child, indent + 1);
        assert(bracket->child->parent == root.get());
    }
}

void printAST(const AST& ast) {
    const std::unique_ptr<ASTNode>& root = ast.root;
    cout << "AST " << ast.anchorBegin << " " << ast.anchorEnd << std::endl;
    _printAST(root, 0);
}


ostream& operator<<(ostream& os, const std::unique_ptr<ASTNode>& root) {
    constexpr const char toescape[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^{|}";  // Keep it sorted
    if (const CharacterMatcher* chr = dynamic_cast<const CharacterMatcher*>(root.get())) {
        char ch = chr->cmatch;
        if (std::binary_search(std::begin(toescape), std::end(toescape), ch))
            os << '\\';
        os << ch;
    } else if (const CharacterClassMatcher* ichr = dynamic_cast<const CharacterClassMatcher*>(root.get())) {
        os << '[';
        if (ichr->invert)
            os << '^';
        for (auto&&interval:ichr->intervals) {
            char ep1 = std::get<0>(interval);
            char ep2 = std::get<1>(interval);
            if (std::binary_search(std::begin(toescape), std::end(toescape), ep1))
                os << '\\';
            os << ep1;
            if (ep1 != ep2) {
                os << '-';
                if (std::binary_search(std::begin(toescape), std::end(toescape), ep2))
                    os << '\\';
                os << ep2;
            }
        }
        os << ']';
    } else if (dynamic_cast<const UniversalMatcher*>(root.get())) {
        os << ".";
    } else if (dynamic_cast<const EpsilonMatcher*>(root.get())) {
        // Output nothing
    } else if (const ConcatenationNode* concat = dynamic_cast<const ConcatenationNode*>(root.get())) {
        for(const auto& child:concat->childs) {
            if (child->priority() > concat->priority()) os << "(";
            os << child;
            if (child->priority() > concat->priority()) os << ")";
            assert(child->parent == root.get());
        }
    } else if (const DisjunctionNode* disj = dynamic_cast<const DisjunctionNode*>(root.get())) {
        for(const auto& child:disj->childs) {
            if (child->priority() > disj->priority()) os << ")";
            os << child;  // Print one of the regexes
            if (child->priority() > disj->priority()) os << ")";
            os << ((&child != &disj->childs.back())?"|":"");  // Not last element
            assert(child->parent == root.get());
        }
    } else if (const KleeneStarNode* kleene = dynamic_cast<const KleeneStarNode*>(root.get())) {
        if (kleene->child->priority() > kleene->priority()) os << "(";
        os << kleene->child;
        if (kleene->child->priority() > kleene->priority()) os << ")";
        os << "*" << ((kleene->greedy)?"":"?");
        assert(kleene->child->parent == root.get());
    } else if (const OneOrMoreNode* oneormore = dynamic_cast<const OneOrMoreNode*>(root.get())) {
        if (oneormore->child->priority() > oneormore->priority()) os << "(";
        os << oneormore->child;
        if (oneormore->child->priority() > oneormore->priority()) os << ")";
        os << "+" << ((oneormore->greedy)?"":"?");
        assert(oneormore->child->parent == root.get());
    } else if (const OneOrNoneNode* oneornone = dynamic_cast<const OneOrNoneNode*>(root.get())) {
        bool lazyc = false;  // Lazy clarification, add parenthesis around what might be intended
        // as a lazy modifier. Chec if the child is any of the nodes who allow a lazy suffix
        // if child is gready, lazyc is set to true, then the parenthesis is printed, else there is no ambiguity
        if (auto ptr = dynamic_cast<KleeneStarNode*>(oneornone->child.get())) { lazyc = ptr->greedy; }
        if (auto ptr = dynamic_cast<OneOrNoneNode*>(oneornone->child.get()))  { lazyc = ptr->greedy; }
        if (auto ptr = dynamic_cast<OneOrMoreNode*>(oneornone->child.get()))  { lazyc = ptr->greedy; }
        if (auto ptr = dynamic_cast<MultiplyNode*>(oneornone->child.get()))   { lazyc = ptr->greedy; }
        if (lazyc || oneornone->child->priority() > oneornone->priority()) os << "(";
        os << oneornone->child;
        if (lazyc || oneornone->child->priority() > oneornone->priority()) os << ")";
        os << "?" << ((oneornone->greedy)?"":"?");
        assert(oneornone->child->parent == root.get());
    } else if (const MultiplyNode* multiply = dynamic_cast<const MultiplyNode*>(root.get())) {
        if (multiply->child->priority() > multiply->priority()) os << "(";
        os << multiply->child;
        if (multiply->child->priority() > multiply->priority()) os << ")";
        os << "{" << multiply->min;
        if (!multiply->exact()) {
            os << ",";
            if (!multiply->unbounded)
                os << multiply->max;
        }
        os << "}" << ((multiply->greedy)?"":"?");
        assert(multiply->child->parent == root.get());
    } else if (const BracketNode* bracket = dynamic_cast<const BracketNode*>(root.get())) {
        os << ((bracket->capture)?"<":"(");
        os << bracket->child;
        os << ((bracket->capture)?">":")");
        assert(bracket->child->parent == root.get());
    }
    return os;
}

ostream& operator<<(ostream& os, const AST& ast) {
    return os << ((ast.anchorBegin)?"^":"") << ast.root << ((ast.anchorEnd)?"$":"");
}

// ==== Generate the Abstract syntax tree ====

template<typename T>
inline void _merge_node_down(std::unique_ptr<ASTNode>& root) {
    if (T* disj = dynamic_cast<T*>(root.get())) {
        for (size_t i = disj->childs.size(); i-- > 0;) {  // reverse loop of the childs
            if (disj->childs[i]->template isinstance<T>()) {
                std::vector<std::unique_ptr<ASTNode>> arg;
                for(auto&child: dynamic_cast<T*>(disj->childs[i].get())->childs)
                    arg.emplace_back(std::move(child));
                
                disj->childs.erase(std::next(disj->childs.begin(), i));  // Remove the node
                size_t argno = 0;
                for(auto&child: arg)  // Inserts the arguments where the node was removed
                    disj->insert_node(i + (argno++), std::move(child));
            }
        }
    }
}

// Merges downward the multiply node when possible
inline void _merge_multiply(std::unique_ptr<ASTNode>& root) {
    if (MultiplyNode* mult = dynamic_cast<MultiplyNode*>(root.get())) {
        if (mult->exact() && mult->child->isinstance<MultiplyNode>()) {
            MultiplyNode* mult2 = dynamic_cast<MultiplyNode*>(mult->child.get());
            if (mult2->exact()) {  // Can merge into the parent
                mult->min *= mult2->min;
                mult->max = mult->min;  // Keeps the exactness
                mult->set_node(std::move(mult2->child));
                assert(mult->exact());
            }
        }
        // If mult is unbounded and begin is 0 or 1 replaces with * or + operator
        if (mult->unbounded && (mult->min == 0 || mult->min == 1)) {
            auto wrapper = [&]() -> std::unique_ptr<ASTNode> {
                if (mult->min == 0) {  // Replace with a star
                    auto w = std::make_unique<KleeneStarNode>(std::move(mult->child));
                    w->greedy = mult->greedy;  // Must be done here: greedy tag is owned by the specialization
                    return w;
                } else if (mult->min == 1) {
                    auto w = std::make_unique<OneOrMoreNode>(std::move(mult->child));
                    w->greedy = mult->greedy;
                    return w;
                } else {  // never happens
                    return nullptr;
                }
            }();
            wrapper->parent = mult->parent;
            root = std::move(wrapper);  // This frees the mult node
        } else if (mult->exact() && mult->min == 0) {  // Substitute with an epsilon
            auto ematch = std::make_unique<EpsilonMatcher>();
            ematch->parent = mult->parent;
            root = std::move(ematch);
        }
    }
}


template<typename T1, typename T2> 
inline void _merge_oneor_hh(std::unique_ptr<ASTNode>& root);
// An helper type to pass parameter packs
template<typename T0, typename... T> struct Helper { using type = T0; };
template<typename... T1s, typename... T2s>
inline void _merge_oneor_h(std::unique_ptr<ASTNode>& root, Helper<T1s...>&&, Helper<T2s...>&&) {
    ( [&](auto dummy) -> void {
        (_merge_oneor_hh<typename decltype(dummy)::type, T2s>(root), ...);
    }(Helper<T1s>{}), ... );
}

template<typename T1, typename T2>  // Merges down
inline void _merge_oneor_hh(std::unique_ptr<ASTNode>& root) {
    bool mergeperformed = false;
    if (T1* element1 = dynamic_cast<T1*>(root.get())) {
        if (T2* element2 = dynamic_cast<T2*>(element1->child.get())) {
            if constexpr(std::is_same<T1, T2>::value) {
                if constexpr(std::is_same<T1, OneOrMoreNode>::value)
                    element1->greedy |= element2->greedy;
                else
                    element1->greedy &= element2->greedy;
                element1->set_node(std::move(element2->child));
                mergeperformed = true;
            } else if constexpr(std::is_same<T1, KleeneStarNode>::value) {
                if constexpr(!std::is_same<T2, OneOrMoreNode>::value)
                    element1->greedy &= element2->greedy;
                element1->set_node(std::move(element2->child));
                mergeperformed = true;
            } else if constexpr(std::is_same<T2, KleeneStarNode>::value) {
                auto parent = root->parent;
                auto childgreedy = element1->greedy;
                root = std::move(element1->child);  // Element1 is freed here
                root->parent = parent;  // Restores the parent node
                auto newelement = dynamic_cast<KleeneStarNode*>(root.get());
                if constexpr(!std::is_same<T1, OneOrMoreNode>::value)
                    newelement->greedy = childgreedy & element2->greedy;
                mergeperformed = true;
            } else {
                constexpr bool C1 = std::is_same<T1, OneOrNoneNode>::value &&
                                    std::is_same<T2, OneOrMoreNode>::value;
                constexpr bool C2 = std::is_same<T1, OneOrMoreNode>::value &&
                                    std::is_same<T2, OneOrNoneNode>::value;
                if ((C1 && (!element1->greedy || element2->greedy)) ||
                    (C2 && (element1->greedy || !element2->greedy)) ) {
                    auto kswrapper = std::make_unique<KleeneStarNode>(std::move(element2->child));
                    kswrapper->parent = element1->parent;
                    kswrapper->greedy = element1->greedy & element2->greedy;
                    root = std::move(kswrapper);
                    mergeperformed = true;
                }
            }
            
            // If any kind of merge has been performed try to merge again downwards
            if (mergeperformed) {  // Root node contains the merge result
                [&](auto helper) {
                    _merge_oneor_h(root, std::forward<decltype(helper)&&>(helper),
                                         std::forward<decltype(helper)&&>(helper));
                }(Helper<KleeneStarNode, OneOrMoreNode, OneOrNoneNode>{});
            }
        }
    }
}

void optimizeAST(std::unique_ptr<ASTNode>& root) {
    if (SingleChildNode* s = dynamic_cast<SingleChildNode*>(root.get()))
        optimizeAST(s->child);  // recursively apply optimization to childs

    if (MultiChildNode* m = dynamic_cast<MultiChildNode*>(root.get()))
        for (auto&&child:m->childs)
            optimizeAST(child);
    
    _merge_node_down<ConcatenationNode>(root);  // Acts only when node is a concatenation node
    _merge_node_down<DisjunctionNode>(root);  // Performs the same operation on disjunction nodes

    _merge_multiply(root);
    [&](auto helper) {
        _merge_oneor_h(root, std::forward<decltype(helper)&&>(helper),
                             std::forward<decltype(helper)&&>(helper));
    }(Helper<KleeneStarNode, OneOrMoreNode, OneOrNoneNode>{});
}

// Returns the root node of an abstract syntax tree
AST buildAST(std::string_view regex, bool optimize=true) {
    AST ast;
    ast.root = std::make_unique<EpsilonMatcher>();  // Matches the empty string
    ast.root->parent = nullptr;
    std::stack<decltype(ast.root)*> activenode;
    activenode.push(&ast.root);
    ssize_t balanced_brackets = 0;
    // Anchors, begin and end:
    std::tie(ast.anchorBegin, ast.anchorEnd) = [&]() -> std::tuple<bool, bool> {
        if (regex.size() > 0) {
            bool anchorbegin = false, anchorend = false;
            if (regex[0] == '^')
                anchorbegin = true;
            if (regex[regex.size()-1] == '$')
                anchorend = true;
            if (anchorbegin) regex.remove_prefix(1);
            if (anchorend) regex.remove_suffix(1);
            return std::make_tuple(anchorbegin, anchorend);
        } else {
            return std::make_tuple(false, false);
        }
    }();

    bool escaped = false;
    bool multiply_environment = false;
    bool multiply_environment_max = false;
    bool multiply_environment_max_str = false;
    bool character_class_environment = false;
    bool character_class_environment_interval = false;
    bool lazymodifier = false;  // Set to true to enable the reading of the lazy modifier
    std::unique_ptr<ASTNode> objbuff = nullptr;  // A buffer object
    for (auto&&c: regex) {
        if (c == '\\' && !escaped) {  // Escape character
            lazymodifier = false;  // No more lazy modifier allowed
            escaped = true;  // Next character is escaped
            continue;  // Exits
        }
        // A special environment to handle character classes
        if (character_class_environment) {
            assert(!multiply_environment && !lazymodifier);  // Environment not allowed
            if ((c == '[' && !escaped)) {  // Not allowed unless escaped
                throw syntax_error("syntax error");
            } else if (!(c == ']' && !escaped)) {  // Still inside the environment
                auto objbuffptr = dynamic_cast<CharacterClassMatcher*>(objbuff.get());
                if (c == '^' && !escaped) {
                    objbuffptr->invert = true;
                } else if (c == '-' && !escaped) {
                    if (objbuffptr->intervals.size())
                        character_class_environment_interval = true;
                    else
                        throw syntax_error("syntax error");
                } else {
                    if (character_class_environment_interval) {
                        size_t last_interval = objbuffptr->intervals.size() - 1;
                        char e = std::get<0>(objbuffptr->intervals[last_interval]);
                        objbuffptr->intervals[last_interval] = std::make_pair(e,c);
                        character_class_environment_interval = false;
                    } else {
                        objbuffptr->intervals.emplace_back(std::make_pair(c,c));
                    }
                }
                escaped = false;
                continue;
            }
        }

        if (c == '[' && !escaped) {  // Enters character class
            character_class_environment = true;
            character_class_environment_interval = false;
            objbuff = std::make_unique<CharacterClassMatcher>();
            lazymodifier = false;
            continue;
        } else if (c == ']' && !escaped) {  // Exits character class
            if (!character_class_environment || character_class_environment_interval)
                throw syntax_error("syntax error");
            character_class_environment = false;
            // Do not continue, this forces to enter the concatenation of the new character
        }
        
        if (multiply_environment) {
            assert(!character_class_environment && !lazymodifier);
            if (escaped) {  // Not allowed escaping in this environment
                throw syntax_error("syntax error");
            } else if (c != '}') {  // Still inside the environment
                if (c == ',') {
                    if (multiply_environment_max)  // More than one maximum
                        throw syntax_error("syntax error");
                    multiply_environment_max = true;
                } else if (c >= '0' && c <= '9') {
                    auto currentnode = dynamic_cast<MultiplyNode*>((*activenode.top()).get());
                    assert(currentnode);
                    int digit = c - '0';
                    if (multiply_environment_max) {  // Store into max
                        multiply_environment_max_str = true;  // Read something as maximum
                        currentnode->max*=10;
                        currentnode->max+=digit;
                    } else {  // Store into min
                        currentnode->min*=10;
                        currentnode->min+=digit;
                    }
                } else if (c != ' ') {  // Ignores spaces
                    throw syntax_error("character not allowed");
                }
                continue;
            }
        }

        if (c == '{' && !escaped) {  // Enters character class
            multiply_environment = true;
            multiply_environment_max = false;
            multiply_environment_max_str = false;
            auto parent = (*activenode.top())->parent;
            std::unique_ptr<ASTNode> wrapper = std::make_unique<MultiplyNode>(std::move(*activenode.top()));
            wrapper->parent = parent;
            *activenode.top() = std::move(wrapper);
            lazymodifier = false;
            continue;
        } else if (c == '}' && !escaped) {  // Exits character class
            if (!multiply_environment)
                throw syntax_error("syntax error");
            auto currentnode = dynamic_cast<MultiplyNode*>((*activenode.top()).get());
            assert(currentnode);
            if (!multiply_environment_max) {  // Only min provided, copy min into max
                currentnode->max = currentnode->min;  // This makes an exact number of matches
            } else if (multiply_environment_max_str) {  // Read a maximum
                if (currentnode->max < currentnode->min)  // max must be > min
                    throw syntax_error("max repetitions less than min repetitions");
            } else { // else unbounded
                currentnode->unbounded = true;
            }
            multiply_environment_max = false;
            multiply_environment = false;
            lazymodifier = true;
            continue;
        }
        
        // True if normal environment
        assert(!character_class_environment && !multiply_environment);
        if (!escaped && (c == ')' || c == '>')) {
            balanced_brackets -= 1;
            if (balanced_brackets < 0)  // Closed more braces than opened
                throw unbalanced_brackets("unbalanced brackets");
            assert(activenode.size() >= 2);
            do {
                activenode.pop();  // Closes all the active branches up to the first parenthesis
            } while ((*activenode.top())->parent &&
                    !(*activenode.top())->isinstance<BracketNode>());
            auto bracket = dynamic_cast<BracketNode*>(activenode.top()->get());
            if ((bracket->capture && c == ')') ||  // Closed a capturing with )
                (!bracket->capture && c == '>'))   // Closed a non-capturing with >
                throw unbalanced_brackets("unbalanced capturing/not capturing groups");
            if (!bracket->capture) {  // Remove non-capturing brackets
                auto parent = (*activenode.top())->parent;
                auto arg = std::move(bracket->child);
                arg->parent = parent;
                *activenode.top() = std::move(arg);
            }
            lazymodifier = false;
        } else if (c == '*' && !escaped) {  // Kleene star
            auto parent = (*activenode.top())->parent;
            auto kswrapper = std::make_unique<KleeneStarNode>(std::move(*activenode.top()));
            kswrapper->parent = parent;
            *activenode.top() = std::move(kswrapper);
            lazymodifier = true;  // Might be followed by a lazy modifier
        } else if (c == '+' && !escaped) {  // One or more
            auto parent = (*activenode.top())->parent;
            auto wrapper = std::make_unique<OneOrMoreNode>(std::move(*activenode.top())); 
            wrapper->parent = parent;
            *activenode.top() = std::move(wrapper);
            lazymodifier = true;  // Might be followed by a lazy modifier
        } else if (c == '?' && !escaped && !lazymodifier) {  // One or none star
            auto parent = (*activenode.top())->parent;
            auto wrapper = std::make_unique<OneOrNoneNode>(std::move(*activenode.top()));    
            wrapper->parent = parent;
            *activenode.top() = std::move(wrapper);
            lazymodifier = true;  // Might be followed by a lazy modifier
        } else if (c == '?' && lazymodifier)  {  // Enables the lazy modifier
            assert(!escaped);  // It should be impossible
            if (auto kleene = dynamic_cast<KleeneStarNode*>((*activenode.top()).get()))
                kleene->greedy = false;
            if (auto oneormore = dynamic_cast<OneOrMoreNode*>((*activenode.top()).get()))
                oneormore->greedy = false;
            if (auto oneornone = dynamic_cast<OneOrNoneNode*>((*activenode.top()).get()))
                oneornone->greedy = false;
            if (auto multiply = dynamic_cast<MultiplyNode*>((*activenode.top()).get()))
                multiply->greedy = false;
            lazymodifier = false;  // Applied, can't be applied a second time
        } else if (c == '|' && !escaped) {
            // union has a lower priority, jumps over all the higher priority operands
            // Since the only lowest priority operand is union we ca just look for it
            while ((*activenode.top())->parent &&
                  !(*activenode.top())->isinstance<DisjunctionNode>() &&
                  !(*activenode.top())->parent->isinstance<BracketNode>()) {
                activenode.pop();  // Move to the upper level
            }
            std::unique_ptr<ASTNode> currmatcher = std::make_unique<EpsilonMatcher>();
            if (!(*activenode.top())->isinstance<DisjunctionNode>()) {  // No disjunction was found
                auto parent = (*activenode.top())->parent;
                std::unique_ptr<ASTNode> cat = std::make_unique<DisjunctionNode>(std::move(*activenode.top()),
                                                                                 std::move(currmatcher));
                cat->parent = parent;
                *activenode.top() = std::move(cat);
                activenode.push(&dynamic_cast<MultiChildNode*>(activenode.top()->get())->childs.back());
            } else {  // A disjunction node has been found, apply associativity
                dynamic_cast<DisjunctionNode*>(activenode.top()->get())->append_node(std::move(currmatcher));
                activenode.push(&dynamic_cast<DisjunctionNode*>(activenode.top()->get())->childs.back());
            }
            lazymodifier = false;
        } else {  // Concatenate operation
            // A matcher of the character to concatenate
            auto currmatcher = [&]() -> std::unique_ptr<ASTNode> {;
                if (!escaped && (c == '(' || c == '<')) {  // Concatenates a bra-ket node
                    balanced_brackets++;
                    auto bracketnode = std::make_unique<BracketNode>(std::make_unique<EpsilonMatcher>());
                    bracketnode->capture = (c == '<');
                    return bracketnode;
                } else if (c == ']' && !escaped) {
                    auto objbuffptr = dynamic_cast<CharacterClassMatcher*>(objbuff.get());
                    assert(objbuffptr);
                    objbuffptr->normalize();
                    if (objbuffptr->empty())
                        throw syntax_error("empty character class");
                    if (objbuffptr->singlec())
                        return std::make_unique<CharacterMatcher>(objbuffptr->character());
                    else
                        return std::move(objbuff);
                } if (c == '.' && !escaped) {
                    return std::make_unique<UniversalMatcher>();
                } else {
                    return std::make_unique<CharacterMatcher>(c);
                }
            }();
            // Check if parent node is a concatenation, if it is moves up the active pointer (associativity)
            if (dynamic_cast<ConcatenationNode*>((*activenode.top())->parent)) {
                activenode.pop();  // Move to the upper level
            }

            if ((*activenode.top())->isinstance<EpsilonMatcher>()) {  // Concatenation with an epsilon
                currmatcher->parent = (*activenode.top())->parent;
                *activenode.top() = std::move(currmatcher);  // Just replaces the result
            } else if ((*activenode.top())->isinstance<ConcatenationNode>()) {
                dynamic_cast<ConcatenationNode*>(activenode.top()->get())->append_node(std::move(currmatcher));
                activenode.push(&dynamic_cast<ConcatenationNode*>(activenode.top()->get())->childs.back());
            } else /* if ((*activenode.top())->isinstance<CharacterMatcher>()) */ {
                auto parent = (*activenode.top())->parent;
                std::unique_ptr<ASTNode> cat = std::make_unique<ConcatenationNode>(std::move(*activenode.top()),
                                                                                   std::move(currmatcher));
                cat->parent = parent;
                *activenode.top() = std::move(cat);
                activenode.push(&dynamic_cast<MultiChildNode*>(activenode.top()->get())->childs.back());
            }

            // If active node is a parenthesis, activates the epsilon-child
            if  ((*activenode.top())->isinstance<BracketNode>()) {
                activenode.push(&dynamic_cast<BracketNode*>(activenode.top()->get())->child);
            }

            lazymodifier = false;  // Lazy modifier no longer allowed
        }
        
        escaped = false;  // Next character is not escaped
    }

    if (balanced_brackets != 0)  // Closed more braces than opened
        throw unbalanced_brackets("unbalanced brackets");
    if (character_class_environment)
        throw syntax_error("character class environment not closed");
    if (multiply_environment)
        throw syntax_error("multiply environment not closed");

    if (optimize)
        optimizeAST(ast.root);
    return ast;
}


bool EqualAST(const std::unique_ptr<ASTNode>& root1,
              const std::unique_ptr<ASTNode>& root2) {
    if (typeid(root1.get()) != typeid(root2.get()))
        return false;
    
    if (root1->isinstance<CharacterMatcher>() && root2->isinstance<CharacterMatcher>()) {
        const CharacterMatcher* chr1 = dynamic_cast<const CharacterMatcher*>(root1.get());
        const CharacterMatcher* chr2 = dynamic_cast<const CharacterMatcher*>(root2.get());
        return chr1->cmatch == chr2->cmatch;
    }

    if (root1->isinstance<CharacterClassMatcher>() && root2->isinstance<CharacterClassMatcher>()) {
        return *dynamic_cast<const CharacterClassMatcher*>(root1.get()) ==
               *dynamic_cast<const CharacterClassMatcher*>(root2.get());
    }

    if (root1->isinstance<UniversalMatcher>() && root2->isinstance<UniversalMatcher>()) return true;
    if (root1->isinstance<EpsilonMatcher>() && root2->isinstance<EpsilonMatcher>()) return true;

    if (root1->isinstance<SingleChildNode>() && root2->isinstance<SingleChildNode>()) {
        const SingleChildNode* c1 = dynamic_cast<const SingleChildNode*>(root1.get());
        const SingleChildNode* c2 = dynamic_cast<const SingleChildNode*>(root2.get());
        if (root1->isinstance<KleeneStarNode>() && root2->isinstance<KleeneStarNode>()) {
            const KleeneStarNode* k1 = dynamic_cast<const KleeneStarNode*>(root1.get());
            const KleeneStarNode* k2 = dynamic_cast<const KleeneStarNode*>(root2.get());
            if (k1->greedy != k2->greedy)
                return false;
        } else if (root1->isinstance<OneOrMoreNode>() && root2->isinstance<OneOrMoreNode>()) {
            const OneOrMoreNode* k1 = dynamic_cast<const OneOrMoreNode*>(root1.get());
            const OneOrMoreNode* k2 = dynamic_cast<const OneOrMoreNode*>(root2.get());
            if (k1->greedy != k2->greedy)
                return false; 
        } else if (root1->isinstance<OneOrNoneNode>() && root2->isinstance<OneOrNoneNode>()) {
            const OneOrNoneNode* k1 = dynamic_cast<const OneOrNoneNode*>(root1.get());
            const OneOrNoneNode* k2 = dynamic_cast<const OneOrNoneNode*>(root2.get());
            if (k1->greedy != k2->greedy)
                return false; 
        } else if (root1->isinstance<MultiplyNode>() && root2->isinstance<MultiplyNode>()) {
            const MultiplyNode* m1 = dynamic_cast<const MultiplyNode*>(root1.get());
            const MultiplyNode* m2 = dynamic_cast<const MultiplyNode*>(root2.get());
            if ((m1->greedy != m2->greedy) || (m1->min != m2->min) ||
                (m1->unbounded != m2->unbounded) || (!m1->unbounded && (m1->max != m2->max)))
                return false;
        } else if (!root1->isinstance<BracketNode>() || !root2->isinstance<BracketNode>()) {
            return false;
        }
        return EqualAST(c1->child, c2->child);
    }

    if (root1->isinstance<MultiChildNode>() && root2->isinstance<MultiChildNode>()) {
        const MultiChildNode* c1 = dynamic_cast<const MultiChildNode*>(root1.get());
        const MultiChildNode* c2 = dynamic_cast<const MultiChildNode*>(root2.get());
        
        if ((root1->isinstance<ConcatenationNode>() && root2->isinstance<ConcatenationNode>()) ||
            (root1->isinstance<DisjunctionNode>() && root2->isinstance<DisjunctionNode>())) {
            if (c1->childs.size() != c2->childs.size()) return false;
            for (size_t i = 0; i < c1->childs.size(); ++i) {
                if (!EqualAST(c1->childs[i], c2->childs[i]))
                    return false;
            }

            return true;
        }
    }

    return false;
}

bool EqualAST(const AST& ast1, const AST& ast2) {
    return ((ast1.anchorBegin == ast2.anchorBegin) &&
            (ast1.anchorEnd == ast2.anchorEnd) && 
             EqualAST(ast1.root, ast2.root));
}


// ========= NFA =========
struct NFAState {
    bool initialState = false;
    bool finalState = false;
    // Matcher, tonode, begin group
    struct transition_info_t {
        template<typename C1, typename C2>
        transition_info_t(const C1& bgroups, const C2& egroups):
            begingroups(bgroups.begin(), bgroups.end()), endgroups(egroups.begin(), egroups.end()) {}
        std::vector<size_t> begingroups;
        std::vector<size_t> endgroups;
    };
    using transition_t = std::tuple<const Matcher*, size_t, std::shared_ptr<const transition_info_t>>;
    using rtransition_t = std::tuple<const Matcher*, size_t, std::shared_ptr<const transition_info_t>>;
    std::vector<transition_t> transitions;  // Matcher, end node
    std::set<rtransition_t> rtransitions;  // a pointer to each reverse transition
};

struct NFA;
NFA ASTtoNFA(const AST& ast, bool optimize);
struct NFA {
    std::vector<NFAState> states;
    std::vector<std::unique_ptr<const Matcher>> matchers;
    size_t nGroups = 1;  // Group 0 always exists
    NFA() = default;
    NFA(const AST& ast, bool optimize=true): NFA(ASTtoNFA(ast, optimize)) {}
    NFA(std::string_view regex, bool optimize=true):
        NFA(buildAST(regex, optimize), optimize) {}

    size_t newState() {  // Creates a new state, and returns it
        states.emplace_back(); // return reference to the last node
        return states.size()-1;  // pointer to the last element
    }

    // Creates a group and returns its index
    size_t newGroup() { return nGroups++; }
    
    template<typename MatcherT>
    void addTransition(MatcherT matcher, size_t fromState, size_t toState,
                       const std::set<size_t>& opengroups = {}, const std::set<size_t>& closegroups = {});

    int optimize();  // Removes some kinds of nodes
    void check() const;  // Asserts the transitions are consistent
    std::vector<std::string_view> simulate(const std::string_view& str) const;
    // bool simulate(const std::string_view& str) const;
    bool powerset(const std::string_view& str) const;
};

void NFA::check() const {
    for (auto& state : states) {
        // Check each transition from the current state
        for (const auto& transition : state.transitions) {
            const Matcher* matcher = std::get<0>(transition);
            size_t endState = std::get<1>(transition);
            std::shared_ptr<const NFAState::transition_info_t> info = std::get<2>(transition);
            assert(endState < states.size());  // End state is a valid state within the NFA
            
            // Check if the matcher transition is a valid transition
            assert(std::find_if(matchers.begin(), matchers.end(), [&](const std::unique_ptr<const Matcher>& ptr) {
                return ptr.get() == matcher;
            }) != matchers.end());

            // Check if it corresponds to a reverse transition
            auto& estate_rt = states[endState].rtransitions;  // reverse transitions of the end state
            size_t ToStateId = (size_t)((NFAState*)&state - &states.front());
            assert(estate_rt.find(std::make_tuple(matcher, ToStateId, info)) != estate_rt.end());

            if (info) {  // nullptr info is allowed
                for (auto&&begingroup:info->begingroups) assert(begingroup < nGroups);
                for (auto&&endgroup:info->endgroups) assert(endgroup < nGroups);
                assert(info->begingroups.size() || info->endgroups.size());
            }
        }

        // Check each reverse transition from the current state
        for (const auto& rtransition: state.rtransitions) {
            const Matcher* matcher = std::get<0>(rtransition);
            size_t startState = std::get<1>(rtransition);
            std::shared_ptr<const NFAState::transition_info_t> info = std::get<2>(rtransition);
            assert(startState < states.size());  // Start state is a valid state within the NFA
            
            // Check if the matcher transition is a valid transition
            assert(std::find_if(matchers.begin(), matchers.end(), [&](const std::unique_ptr<const Matcher>& ptr) {
                return ptr.get() == matcher;
            }) != matchers.end());

            // Check if it corresponds to a direct transition
            auto& estate_rt = states[startState].transitions;
            size_t FromStateId = (size_t)((NFAState*)&state - &states.front());
            assert(std::count(std::begin(estate_rt), std::end(estate_rt),
                              std::make_tuple(matcher, FromStateId, info)) == 1);
            
            if (info) {  // nullptr info is allowed
                for (auto&&begingroup:info->begingroups) assert(begingroup < nGroups);
                for (auto&&endgroup:info->endgroups) assert(endgroup < nGroups);
                assert(info->begingroups.size() || info->endgroups.size());
            }
        }
    }
}

template<typename MatcherT>
void NFA::addTransition(MatcherT matcher, size_t fromState, size_t toState,
                        const std::set<size_t>& opengroups, const std::set<size_t>& closegroups) {
    bool useinfo = opengroups.size() || closegroups.size();  // If neither has elements do not allocate memory
    auto info = (useinfo)?std::make_shared<NFAState::transition_info_t>(opengroups, closegroups):
                          std::shared_ptr<NFAState::transition_info_t>(nullptr);
    auto& tmatcher = matchers.emplace_back(std::move(matcher));  // Adds the matcher
    states[fromState].transitions.emplace_back(tmatcher.get(), toState, info);
    states[toState].rtransitions.emplace(tmatcher.get(), fromState, info);
    check();
}

void _ASTtoNFA(NFA& nfa, size_t begin, size_t end, const std::unique_ptr<ASTNode>& root,
               const std::set<size_t>& opengroups = {}, const std::set<size_t>& closegroups = {}) {
    if (const Matcher* chr = dynamic_cast<const Matcher*>(root.get())) {
        nfa.addTransition(chr->clone(), begin, end, opengroups, closegroups);
    } else if (const ConcatenationNode* concat = dynamic_cast<const ConcatenationNode*>(root.get())) {
        size_t newbegin = begin;
        for (size_t i = 0; i < concat->childs.size(); i++) {
            size_t newend = (i != concat->childs.size()-1)?nfa.newState():end;
            const std::set<size_t>& newogroups = (i==0)?opengroups:std::set<size_t>();
            const std::set<size_t>& newcgroups = (i == concat->childs.size()-1)?closegroups:std::set<size_t>();
            _ASTtoNFA(nfa, newbegin, newend, concat->childs[i], newogroups, newcgroups);
            newbegin = newend;  // the begin of the next node is the end of the prior
        }
    } else if (const DisjunctionNode* disj = dynamic_cast<const DisjunctionNode*>(root.get())) {
        for (auto&child:disj->childs)
            _ASTtoNFA(nfa, begin, end, child, opengroups, closegroups);
    } else if (const KleeneStarNode* kleene = dynamic_cast<const KleeneStarNode*>(root.get())) {
        if (kleene->child->accept_epsilon()) {
            size_t before = nfa.newState();
            size_t after = nfa.newState();
            if (kleene->greedy) {
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, before, opengroups, {});
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, end, opengroups, closegroups);
            } else {
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, end, opengroups, closegroups);
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, before, opengroups, {});
            }
            _ASTtoNFA(nfa, before, after, kleene->child, {}, {});  // Adds the child to the nfa
            if (kleene->greedy) {  // First try to stay inside
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
            } else {  // The same, but in reverse
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});
            }
        } else {
            size_t mid = nfa.newState();
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, mid, opengroups, {});
            if (kleene->greedy) {
                _ASTtoNFA(nfa, mid, mid, kleene->child, {}, {});  // Adds the child to the nfa
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), mid, end, {}, closegroups);
            } else {  // The same, but in reverse
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), mid, end, {}, closegroups);
                _ASTtoNFA(nfa, mid, mid, kleene->child, {}, {});  // Adds the child to the nfa    
            }
        }
    } else if (const OneOrMoreNode* oneormore = dynamic_cast<const OneOrMoreNode*>(root.get())) {
        size_t before = nfa.newState();
        size_t after = nfa.newState();
        nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, before, opengroups, {});
        _ASTtoNFA(nfa, before, after, oneormore->child, {}, {});  // Adds the child to the nfa
        if (oneormore->greedy) {
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});  // Greedy, stay inside if possible
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
        } else {  // Lazy version, first tries to exit, then to stay inside
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});
        }
    } else if (const OneOrNoneNode* oneornone = dynamic_cast<const OneOrNoneNode*>(root.get())) {
        if (oneornone->greedy) {
            _ASTtoNFA(nfa, begin, end, oneornone->child, opengroups, closegroups);  // Matches one
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, end, opengroups, closegroups);  // Matches none
        } else {  // Lazy version
            nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, end, opengroups, closegroups);
            _ASTtoNFA(nfa, begin, end, oneornone->child, opengroups, closegroups);  // Matches the regex after
        }
    } else if (const MultiplyNode* multiply = dynamic_cast<const MultiplyNode*>(root.get())) {
        size_t newbegin = begin;
        size_t i = 0;

        if (multiply->min != 0) {
            for (; i < multiply->min-1; i++) {
                size_t newend = nfa.newState();
                const std::set<size_t>& newogroups = (i==0)?opengroups:std::set<size_t>();
                _ASTtoNFA(nfa, newbegin, newend, multiply->child, newogroups, {});
                newbegin = newend;  // the begin of the next node is the end of the prior
            }
        }
        
        if (multiply->exact()) {  // Exact number of matches
            if (multiply->min != 0) { // Add the last state
                const std::set<size_t>& newogroups = (i==0)?opengroups:std::set<size_t>();
                _ASTtoNFA(nfa, newbegin, end, multiply->child, newogroups, closegroups);
            } else { // Exactly zero, add an epsilon transition
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, end, opengroups, closegroups);
            }
        } else if (multiply->unbounded) {
            if (multiply->min == 0) {  // This is exactly a star node
                size_t mid = nfa.newState();
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, mid, opengroups, {});
                if (multiply->greedy) {
                    _ASTtoNFA(nfa, mid, mid, multiply->child, {}, {});  // Adds the child to the nfa
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), mid, end, {}, closegroups);
                } else {  // The same, but in reverse
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), mid, end, {}, closegroups);
                    _ASTtoNFA(nfa, mid, mid, multiply->child, {}, {});  // Adds the child to the nfa    
                }
            } else if (multiply->min == 1) {  // This is a plus node
                size_t before = nfa.newState();
                size_t after = nfa.newState();
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), begin, before, opengroups, {});
                _ASTtoNFA(nfa, before, after, multiply->child, {}, {});  // Adds the child to the nfa
                if (multiply->greedy) {
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});  // Greedy, stay inside if possible
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
                } else {  // Lazy version, first tries to exit, then to stay inside
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, end, {}, closegroups);
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), after, before, {}, {});
                }
            } else {   // Normal case, I can make an epsilon transition to begin-node
                size_t newend = nfa.newState();
                assert(i!=0);
                _ASTtoNFA(nfa, newbegin, newend, multiply->child, {}, {});
                if (multiply->greedy) {
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), newend, newbegin, {}, {});
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), newend, end, {}, closegroups);
                } else {  // Lazy
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), newend, end, {}, closegroups);
                    nfa.addTransition(std::make_unique<EpsilonMatcher>(), newend, newbegin, {}, {});
                }
            }
        } else {  // Bounded, not exact
            for (; i < multiply->max-1; i++) {
                size_t newend = nfa.newState();
                const std::set<size_t>& newogroups = (i==0)?opengroups:std::set<size_t>();
                if (multiply->greedy) {
                    _ASTtoNFA(nfa, newbegin, newend, multiply->child, newogroups, {});
                    if (i >= multiply->min)
                        nfa.addTransition(std::make_unique<EpsilonMatcher>(), newbegin, end, newogroups, closegroups);
                } else {
                    if (i >= multiply->min)
                        nfa.addTransition(std::make_unique<EpsilonMatcher>(), newbegin, end, newogroups, closegroups);
                    _ASTtoNFA(nfa, newbegin, newend, multiply->child, newogroups, {});
                }
                newbegin = newend;
            }
            const std::set<size_t>& newogroups = (i==0)?opengroups:std::set<size_t>();
            if (multiply->greedy) {
                _ASTtoNFA(nfa, newbegin, end, multiply->child, newogroups, closegroups);
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), newbegin, end, newogroups, closegroups);
            } else {
                nfa.addTransition(std::make_unique<EpsilonMatcher>(), newbegin, end, newogroups, closegroups);
                _ASTtoNFA(nfa, newbegin, end, multiply->child, newogroups, closegroups);
            }
        }
    } else if (const BracketNode* bracket = dynamic_cast<const BracketNode*>(root.get())) {
        std::set<size_t> ogroups, cgroups;  // Temporary groups, filled only if necessary
        const auto& [newopengroups, newclosegroups] = [&]() ->  // Lambda returning pair of ref
                std::tuple<const std::set<size_t>&, const std::set<size_t>&> {
            if (bracket->capture) {
                size_t newGroupid = nfa.newGroup();
                ogroups = std::set<size_t>(opengroups.begin(), opengroups.end());
                cgroups = std::set<size_t>(closegroups.begin(), closegroups.end());
                ogroups.emplace(newGroupid);
                cgroups.emplace(newGroupid);
                return std::tie(ogroups, cgroups);
                // return std::tie(opengroups, closegroups);
            } else {
                return std::tie(opengroups, closegroups);
            }
        }();
        _ASTtoNFA(nfa, begin, end, bracket->child, newopengroups, newclosegroups);
    }
}

NFA ASTtoNFA(const AST& ast, bool optimize=true) {
    const std::unique_ptr<ASTNode>& root = ast.root;
    NFA nfa;
    size_t begin = nfa.newState(), end = nfa.newState();
    nfa.states[begin].initialState = true;
    nfa.states[end].finalState = true;
    _ASTtoNFA(nfa, begin, end, root, {0}, {0});
    if (!ast.anchorBegin) nfa.addTransition(std::make_unique<UniversalMatcher>(), begin, begin, {}, {});
    if (!ast.anchorEnd) nfa.addTransition(std::make_unique<UniversalMatcher>(), end, end, {}, {});
    if (optimize)
        nfa.optimize();
    nfa.check();
    return nfa;
}

int NFA::optimize() {  // This is not a minimize
    std::function<void(size_t, size_t)> remove_node = [this](size_t i, size_t j) {
        this->states.erase(this->states.begin() + i);
        bool unique = i == j;  // if i == j we are removing unique nodes
        j -= (j > i);  // Subtracts 1 if j > i. That is the new index for j
        // Must update all the transitions of the remaining nodes:
        for (auto&node:this->states) {
            for (auto&transition:node.transitions) {
                if (unique) assert(std::get<1>(transition) != i); // No transitions from i
                if (std::get<1>(transition) > i)
                    std::get<1>(transition)--;
                else if (std::get<1>(transition) == i)  // Redirect transitions to i to the node j
                    std::get<1>(transition) = j;
            }
            std::set<NFAState::rtransition_t> edit;
            for (auto&rtransition:node.rtransitions) {
                const Matcher* matcher = std::get<0>(rtransition);
                size_t fromState = std::get<1>(rtransition);
                std::shared_ptr<const NFAState::transition_info_t> info = std::get<2>(rtransition);
                if (unique) assert(fromState != i); // No transitions toward i
                if (fromState > i)
                    edit.emplace(matcher, fromState-1, info);
                else if (fromState < i)
                    edit.emplace(matcher, fromState, info);
                else
                    edit.emplace(matcher, j, info);
            }
            node.rtransitions.swap(edit);
        }
    };

    size_t initialnodes = this->states.size();
    // Look for states reached by a single epsilon transition
    for (size_t i = this->states.size(); i-- > 0;) {
        auto&state = this->states[i];
        if (state.rtransitions.size() == 0 && !state.initialState) {
            // Unreachable node, can remove altogether
            remove_node(i, i);
        } else if (state.rtransitions.size() == 1 && !state.initialState) {
            const Matcher * matcher = std::get<0>(*state.rtransitions.begin());
            std::shared_ptr<const NFAState::transition_info_t> info = std::get<2>(*state.rtransitions.begin());
            if (dynamic_cast<const EpsilonMatcher*>(matcher) && info == nullptr) {
                size_t j = std::get<1>(*state.rtransitions.begin());
                auto& trans = states[j].transitions;
                auto result = std::find(std::begin(trans), std::end(trans), std::make_tuple(matcher, i, info));
                std::size_t insert_index = std::distance(trans.begin(), result);
                assert(result != trans.end());
                trans.erase(result);  // Erase the transaction towards the node to be deleted
                trans.insert(trans.begin() + insert_index, state.transitions.begin(), state.transitions.end());
                
                remove_node(i, j);  // Merges the two nodes, re-route i to j
            }
        }
    }

    // Look for states reached by a single epsilon transition
    for (size_t i = this->states.size(); i-- > 0;) {
        auto&state = this->states[i];
        if (state.transitions.size() == 0 && !state.finalState) {
            // Dead end node, can remove altogether
            remove_node(i, i);
        } else if (state.transitions.size() == 1 && !state.finalState) {
            const Matcher * matcher = std::get<0>(*state.transitions.begin());
            std::shared_ptr<const NFAState::transition_info_t> info = std::get<2>(*state.transitions.begin());
            if (dynamic_cast<const EpsilonMatcher*>(matcher) && info == nullptr) {
                size_t j = std::get<1>(*state.transitions.begin());
                assert(states[j].rtransitions.erase(std::make_tuple(matcher, i, info)) > 0);
                states[j].rtransitions.insert(state.rtransitions.begin(), state.rtransitions.end());
                
                remove_node(i, j);  // Merges the two nodes, re-route i to j
            }
        }
    }
    return initialnodes - this->states.size();
}

std::vector<std::string_view> NFA::simulate(const std::string_view& str) const {
    std::set<std::pair<size_t, size_t>> visitedStates;
    std::vector<std::string_view> captures(nGroups);

    // Define a helper recursive function to explore possible transitions
    std::function<bool(size_t, const std::string_view&)> exploreTransitions = [&](size_t currentState, const std::string_view& remainingStr) {
        // Base case: If the remaining string is empty and the current state is a final state, we have a match
        
        if (remainingStr.empty() && states[currentState].finalState)
            return true;

        std::pair<size_t, size_t> currentStatePosition = std::make_pair(currentState, str.size() - remainingStr.size());        
        if (visitedStates.find(currentStatePosition) != visitedStates.end())
            return false; // We have visited this state with the same input position before
        visitedStates.insert(currentStatePosition);

        // Iterate over the transitions from the current state
        for (const auto& currtransition : states[currentState].transitions) {
            const auto& [matcher, nextState, info] = currtransition;
            if (matcher->match(remainingStr)) {  // I can take this path
                
                // Handles capturing groups info
                std::vector<std::string_view> temp_captures;
                if (info) {  // Contains informations about start and end groups
                    temp_captures = std::vector<std::string_view>(captures.begin(), captures.end());
                    for (auto&begingroup:info->begingroups)
                        captures[begingroup] = {&remainingStr[0], 0};
                    for (auto&endgroup:info->endgroups) {
                        auto data = captures[endgroup].data();
                        captures[endgroup] = {data, (size_t)(&remainingStr[matcher->length()]-data)};
                    }
                }

                // Recur to the next state with the remaining string after consuming the matched characters
                if (exploreTransitions(nextState, remainingStr.substr(matcher->length()))) {
                    return true; // We have a successful match
                }

                // If transition is not successful restore capturing info
                if (info) {  // Contains informations about start and end groups
                    std::copy(temp_captures.begin(), temp_captures.end(), captures.begin());
                }
            }
        }

        return false; // No match found from the current state
    };

    // Start the exploration from all initial states
    for (auto&state : states) {
        size_t stateId = &state - &states.front();
        if (state.initialState && exploreTransitions(stateId, str))
            return captures; // We found a match from one of the initial states
    }
    return {}; // No match found, return an empty captures set
}



bool NFA::powerset(const std::string_view& str) const {
    std::set<size_t> currentStates; // Set of current states
    for (auto&state : states) {
        size_t stateId = &state - &states.front();
        if (state.initialState)
            currentStates.insert(stateId); // Start with the initial state (state 0)
    }


    std::function<void(const NFA&, std::set<size_t>&)> epsilonClosure = [&](const NFA& nfa, std::set<size_t>& inputStates) {
        std::stack<size_t> stateStack;

        // Initialize the stack with the input states
        for (size_t state : inputStates)
            stateStack.push(state);

        while (!stateStack.empty()) {
            size_t currentState = stateStack.top();
            stateStack.pop();

            // Find epsilon transitions from the current state
            for (const auto& transition : nfa.states[currentState].transitions) {
                if (dynamic_cast<const EpsilonMatcher*>(std::get<0>(transition))) {
                    size_t nextState = std::get<1>(transition);
                    // If the next state is not already in the closure, add it and push it to the stack
                    if (inputStates.find(nextState) == inputStates.end()) {
                        inputStates.insert(nextState);
                        stateStack.push(nextState);
                    }
                }
            }
        }
    };

    // Process each character in the input string
    for (char c : str) {
        // Calculate the set of states reachable from currentStates using only epsilon transitions
        epsilonClosure(*this, currentStates);
        // Check if any of the resulting states are final states

        std::set<size_t> newStates; // Set to store the next states after consuming c
        // Calculate the set of states reachable by consuming character c
        for (size_t state : currentStates) {
            for (const auto& transition : states[state].transitions) {
                assert(std::get<0>(transition)->length() <= 1);  // Only transitions supported
                if (std::get<0>(transition)->length() == 1 &&
                    std::get<0>(transition)->match(std::string_view(&c, 1))) {
                    newStates.insert(std::get<1>(transition));
                }
            }
        }
        
        // Update the current set of states
        currentStates = std::move(newStates);
    }

    epsilonClosure(*this, currentStates);

    // Check if any of the resulting states are final states
    for (size_t state : currentStates)
        if (states[state].finalState)
            return true; // Match succeeds
    
    return false; // Match fails
}

ostream& operator<<(ostream& os, const Matcher& match) {
    constexpr const char toescape[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^{|}";  // Keep it sorted
    if (dynamic_cast<const EpsilonMatcher*>(&match)) {
        os << "epsilon";
    } else if (dynamic_cast<const UniversalMatcher*>(&match)) {
        os << "universal";
    } else if (const CharacterMatcher* chr = dynamic_cast<const CharacterMatcher*>(&match)) {
        os << "character ";
        if (std::binary_search(std::begin(toescape), std::end(toescape), chr->cmatch))
            os << '\\';
        os << chr->cmatch;
    } else if (const CharacterClassMatcher* cclass = dynamic_cast<const CharacterClassMatcher*>(&match)) {
        os << ((cclass->invert)?"i":"") << "class ";
        for (auto&&interval:cclass->intervals) {
            char ep1 = std::get<0>(interval);
            char ep2 = std::get<1>(interval);
            if (std::binary_search(std::begin(toescape), std::end(toescape), ep1))
                os << '\\';
            os << ep1;
            if (ep1 != ep2) {
                os << '-';
                if (std::binary_search(std::begin(toescape), std::end(toescape), ep2))
                    os << '\\';
                os << ep2;
            }
        }
    }
    return os;
}

void PrintINFO(const NFAState::transition_info_t* info) {
    if (info) {
        if (info->begingroups.size() != 0) {
            cout << " bgs:";
            for (auto&&begingroup:info->begingroups)
                cout << " " << begingroup;
        }
        if (info->endgroups.size() != 0) {
            cout << " egs:";
            for (auto&&endgroup:info->endgroups)
                cout << " " << endgroup;
        }
    }
}

void PrintNFA(const NFA& nfa) {
    for (auto&&state:nfa.states) {
        size_t id = &state - &nfa.states.front();
        // size_t id = &state - &nfa.states.front();
        cout << "Node " << id << ((state.initialState)?" initial":"") << ((state.finalState)?" final":"") << endl;
        for (auto&&transition:state.transitions) {
            size_t toid = std::get<1>(transition);
            cout << "    -> " << toid << ": " << *std::get<0>(transition);
            PrintINFO(std::get<2>(transition).get());
            cout << endl;
        }
        for (auto&&rtransition:state.rtransitions) {
            size_t fromid = std::get<1>(rtransition);
            cout << "    <- " << fromid << ": " << *std::get<0>(rtransition);
            PrintINFO(std::get<2>(rtransition).get());
            cout << endl;
        }
    }
}



int main() {
    std::cout << " ==== EMAILS ==== " << std::endl;
    auto emailmatcher = NFA("<[a-zA-Z0-9._%+\\-]+>@<[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}>");
    std::vector<std::string_view> emails = {"contact@mywebsite.io", "randomemailaddress",
                        "john.doe@example.com", "emailaddress123@", "support.team@123-xyz.org",
                        "mary@some-provider.net", "email@regexexample", "john.doe123@test",
                        "info@company.co.uk", "@example.com", "hello.world@developers.com",
                        "jennifer.smith123@gmail.com", "regextest@random", "testemail@regex"};
    for (auto&&email:emails) {  // For each chandidate email
        std::cout << "String: " << email << std::endl;
        auto isemail = emailmatcher.powerset(email);
        std::cout << "   Is it an email address?   " << ((isemail)?"Yes":"No") << std::endl;
        if (isemail) {
            auto captures = emailmatcher.simulate(email);
            assert(captures.size() == 3);
            std::cout << "   Username   :              " << captures[1] << std::endl;
            std::cout << "   Domain name:              " << captures[2] << std::endl;
        }
        std::cout << std::endl;
    }

    std::cout << " ==== URLS ==== " << std::endl;
    auto urlmatcher = NFA("^<[_a-zA-Z0-9\\-]+>://(<[^@:/]+>(:<[^@:/]+>)?@)?<[^@:/]+\\.[^@:/]+>(:<[0-9]+>)?(/<.*?>(\\?<.*>)?)?$");
    std::vector<std::string_view> urls = {"http://blog.example.org:8080/archive.html", "http//john.doe@example.org/doc.html",
        "https.profile.example.com/user.html", "https://www.google.com/search.html?q=keyword", "http://example/page.html",
        "http://www.example.com/index.html", "ftp://user:password@myserver.net:8080/home.html", "wwwgooglecom/search.html",
        "https://www.wikipedia.org/about.html", "www.*$@.com/index.html?filter=price", "https://www.facebook.com/profile.html",
        "blog.examplecom/archive.html", "ftp://files.example.com:2121/document.pdf", "ftp:/myfiles.net:2121/files.html"};
    for (auto&&url:urls) {  // For each chandidate email
        std::cout << "String: " << url << std::endl;
        auto isurl = urlmatcher.powerset(url);
        std::cout <<     "   Is it an url?   " << ((isurl)?"Yes":"No") << std::endl;
        if (isurl) {
            auto captures = urlmatcher.simulate(url);
            assert(captures.size() == 8);
            std::cout << "   Protocol:       " << captures[1] << std::endl;
            std::cout << "   User:           " << captures[2] << std::endl;
            std::cout << "   Password:       " << captures[3] << std::endl;
            std::cout << "   Domain name:    " << captures[4] << std::endl;
            std::cout << "   Port:           " << captures[5] << std::endl;
            std::cout << "   Path:           " << captures[6] << std::endl;
            std::cout << "   Query:          " << captures[7] << std::endl;
        }
        std::cout << std::endl;
    }

    // Test 2: check optimizations do not change the functionality
    std::function<std::vector<std::string>(const std::string&)> readFile =
            [&](const std::string& filename) -> std::vector<std::string> {
        std::ifstream inputFile(filename); // Replace "input.txt" with your file name
        if (!inputFile) {
            std::cerr << "Failed to open the file: " << filename << std::endl;
            return {};  // Empty vector
        }

        int N;
        if (!(inputFile >> N)) {
            std::cerr << "Failed to read the input size, file: " << filename << std::endl;
            return {};
        }

        std::vector<std::string> lines(N);
        std::string line;
        std::getline(inputFile, line); // Clear the newline character after reading N
        
        for (int i = 0; i < N; i++) {
            if (!std::getline(inputFile, line)) {
                std::cerr << "Failed to read line " << i + 1 << "." 
                          << " file: " << filename << std::endl;
                return {};
            }
            lines[i] = line;
        }
        return lines;
    };

    auto regexes = readFile("regexes.txt");
    auto inputs = readFile("inputs.txt");

    std::cout << std::boolalpha;
    for (auto&&regex:regexes) {
        // Checks the regex is read and printed correctly (read, print, read, check)
        auto ast = buildAST(regex);
        std::stringstream ss, ss_check;
        ss << ast;
        // std::cout << ss.str() << std::endl;
        auto ast_check = buildAST(ss.str());
        ss_check << ast_check;
        // std::cout << ss_check.str() << std::endl;
        // std::cout << regex << std::endl;
        assert(ss.str() == ss_check.str());    
        // printAST(ast_check);
        // printAST(ast);
        assert(EqualAST(ast_check, ast));
        // std::cout << "===========" << std::endl;        
        // printAST(ast_check);
        // printAST(ast);
        // std::cout << std::boolalpha << EqualAST(ast_check, ast) << std::endl;
        
        auto ast2 = buildAST(regex, false);  // Not optimized ast
        auto nfa = ASTtoNFA(ast2, false);  // Do not optimize the nfa
        ast2.optimize();  // Removes unnecessary nodes
        auto nfa2 = ASTtoNFA(ast2);  // Do optimize the nfa
        // PrintNFA(nfa2);
        // PrintNFA(nfa2);
        
        for (const auto&input:inputs) {
            auto inputsw = std::string_view(input);
            auto captures  = nfa .simulate(inputsw);
            auto captures2 = nfa2.simulate(inputsw);

            bool result  = captures .size() != 0;
            bool result2 = captures2.size() != 0;

            bool result_powerset  = nfa .powerset(inputsw);
            bool result_powerset2 = nfa2.powerset(inputsw);

            assert(result  == result_powerset );
            assert(result2 == result_powerset2);

            if (inputsw.empty())
                assert(ast.root->accept_epsilon() == result);

            if (result != result2 || ((captures.size() && captures2.size()) && captures[0] != captures2[0])) {
                std::cout << "NOT OPTIMIZED: " << ss.str() << std::endl;
                std::cout << "    OPTIMIZED: " << ast << std::endl;
                std::cout << "INPUT: " << input << std::endl;
                std::cout << "OUTPUT: non-o: " << result << "  --  opt: " << result2 << std::endl;
                std::cout << "NON Optimized captured: ";
                if (captures.size()) {
                    if (captures[0].empty())
                        std::cout << "empty";
                    else
                        std::cout << std::distance(std::begin(inputsw), captures[0].begin()) << "-" <<
                                    std::distance(std::begin(inputsw), captures[0].end()) << " " <<
                                    captures[0];
                } else {
                    std::cout << "nothing";
                }
                std::cout << std::endl;
                std::cout << "    Optimized captured: ";
                if (captures2.size()) {
                    if (captures2[0].empty())
                        std::cout << "empty";
                    else
                        std::cout << std::distance(std::begin(inputsw), captures2[0].begin()) << "-" <<
                                    std::distance(std::begin(inputsw), captures2[0].end()) << " " <<
                                    captures2[0];
                } else {
                    std::cout << "nothing";
                }
                std::cout << std::endl;
                // printAST(ast);
               
                PrintNFA(nfa);
                PrintNFA(nfa2);
                nfa2.check();
                std::cout << "=============== " << std::endl;
            }
        }

        /*
        std::cout << "DEBUG: " << std::endl;
        for (size_t i = 0; i < captures.size(); i++) {
            std::cout << "GROUP " << i << " ";
            if (captures[i].empty())
                std::cout << "empty";
            else
                std::cout << std::distance(std::begin(input), captures[i].begin()) << "-" <<
                             std::distance(std::begin(input), captures[i].end()) << " " <<
                             captures[i];
            std::cout << std::endl;
        }
        */
    }
    return 0;
}
