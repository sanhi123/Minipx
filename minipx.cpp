// minipx: tiny expression compiler -> PTX-style assembly
// Pipeline: lexer -> recursive-descent parser -> AST -> constant folding -> codegen
// Usage: ./minipx input.txt [--no-fold]
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;

// ---------- Lexer ----------
enum class T { Num, Id, Op, Assign, Semi, LP, RP, End };
struct Tok { T t; string s; };

vector<Tok> lex(const string& src) {
    vector<Tok> out;
    size_t i = 0;
    while (i < src.size()) {
        char c = src[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && isdigit((unsigned char)src[j])) j++;
            out.push_back({T::Num, src.substr(i, j - i)}); i = j;
        } else if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            out.push_back({T::Id, src.substr(i, j - i)}); i = j;
        } else if (string("+-*/").find(c) != string::npos) { out.push_back({T::Op, string(1, c)}); i++; }
        else if (c == '=') { out.push_back({T::Assign, "="}); i++; }
        else if (c == ';') { out.push_back({T::Semi, ";"}); i++; }
        else if (c == '(') { out.push_back({T::LP, "("}); i++; }
        else if (c == ')') { out.push_back({T::RP, ")"}); i++; }
        else throw runtime_error(string("lex error: unexpected '") + c + "'");
    }
    out.push_back({T::End, ""});
    return out;
}

// ---------- AST ----------
struct Node;
using NP = unique_ptr<Node>;
struct Node {
    enum Kind { Num, Var, Bin } kind;
    int val = 0;          // Num
    string name;          // Var
    char op = 0;          // Bin
    NP l, r;
};
NP num(int v) { auto n = make_unique<Node>(); n->kind = Node::Num; n->val = v; return n; }
NP var(const string& s) { auto n = make_unique<Node>(); n->kind = Node::Var; n->name = s; return n; }
NP bin(char op, NP l, NP r) {
    auto n = make_unique<Node>(); n->kind = Node::Bin; n->op = op;
    n->l = move(l); n->r = move(r); return n;
}
struct Stmt { string target; NP expr; };

// ---------- Parser (recursive descent) ----------
// stmt   := ID '=' expr ';'
// expr   := term (('+'|'-') term)*
// term   := factor (('*'|'/') factor)*
// factor := NUM | ID | '(' expr ')' | '-' factor
struct Parser {
    vector<Tok> toks; size_t p = 0;
    const Tok& cur() { return toks[p]; }
    Tok eat(T t, const char* what) {
        if (cur().t != t) throw runtime_error(string("parse error: expected ") + what);
        return toks[p++];
    }
    bool isOp(const char* s) { return cur().t == T::Op && cur().s == s; }

    vector<Stmt> program() {
        vector<Stmt> v;
        while (cur().t != T::End) {
            string id = eat(T::Id, "identifier").s;
            eat(T::Assign, "'='");
            NP e = expr();
            eat(T::Semi, "';'");
            v.push_back({id, move(e)});
        }
        return v;
    }
    NP expr() {
        NP l = term();
        while (isOp("+") || isOp("-")) { char op = toks[p++].s[0]; l = bin(op, move(l), term()); }
        return l;
    }
    NP term() {
        NP l = factor();
        while (isOp("*") || isOp("/")) { char op = toks[p++].s[0]; l = bin(op, move(l), factor()); }
        return l;
    }
    NP factor() {
        if (cur().t == T::Num) return num(stoi(toks[p++].s));
        if (cur().t == T::Id) return var(toks[p++].s);
        if (cur().t == T::LP) { p++; NP e = expr(); eat(T::RP, "')'"); return e; }
        if (isOp("-")) { p++; return bin('-', num(0), factor()); }
        throw runtime_error("parse error: unexpected token '" + cur().s + "'");
    }
};

// ---------- Optimization: constant folding + algebraic simplification ----------
int foldCount = 0;
NP fold(NP n) {
    if (n->kind != Node::Bin) return n;
    n->l = fold(move(n->l));
    n->r = fold(move(n->r));
    bool lc = n->l->kind == Node::Num, rc = n->r->kind == Node::Num;
    if (lc && rc) {
        int a = n->l->val, b = n->r->val;
        if (n->op == '/' && b == 0) throw runtime_error("division by zero in constant expression");
        int v = n->op == '+' ? a + b : n->op == '-' ? a - b : n->op == '*' ? a * b : a / b;
        foldCount++;
        return num(v);
    }
    // identities: x+0, 0+x, x-0, x*1, 1*x, x/1 -> x ; x*0, 0*x -> 0
    if ((n->op == '+' && rc && n->r->val == 0) || (n->op == '-' && rc && n->r->val == 0) ||
        (n->op == '*' && rc && n->r->val == 1) || (n->op == '/' && rc && n->r->val == 1)) { foldCount++; return move(n->l); }
    if ((n->op == '+' && lc && n->l->val == 0) || (n->op == '*' && lc && n->l->val == 1)) { foldCount++; return move(n->r); }
    if (n->op == '*' && ((lc && n->l->val == 0) || (rc && n->r->val == 0))) { foldCount++; return num(0); }
    return n;
}

// ---------- Codegen: PTX-style, virtual registers ----------
struct CodeGen {
    vector<string> body;
    map<string, string> varReg;   // current register holding each variable
    vector<string> params;        // free variables -> kernel params
    int nextReg = 0;
    string newReg() { return "%r" + to_string(++nextReg); }

    string gen(const Node* n) {   // returns operand: register or immediate
        if (n->kind == Node::Num) return to_string(n->val);
        if (n->kind == Node::Var) {
            auto it = varReg.find(n->name);
            if (it != varReg.end()) return it->second;
            string r = newReg();  // undefined variable: treat as input parameter
            params.push_back(n->name);
            body.push_back("    ld.param.s32 " + r + ", [" + n->name + "];");
            varReg[n->name] = r;
            return r;
        }
        string a = gen(n->l.get()), b = gen(n->r.get());
        string d = newReg();
        const char* ins = n->op == '+' ? "add.s32" : n->op == '-' ? "sub.s32" : n->op == '*' ? "mul.lo.s32" : "div.s32";
        body.push_back(string("    ") + ins + " " + d + ", " + a + ", " + b + ";");
        return d;
    }
    void stmt(const Stmt& s) {
        string v = gen(s.expr.get());
        if (v[0] != '%') {        // immediate -> materialize in a register
            string r = newReg();
            body.push_back("    mov.s32 " + r + ", " + v + ";");
            v = r;
        }
        body.push_back("    // " + s.target + " -> " + v);
        varReg[s.target] = v;
    }
    string emit() {
        ostringstream o;
        o << ".visible .entry kernel(";
        for (size_t i = 0; i < params.size(); i++) o << (i ? ", " : "") << ".param .s32 " << params[i];
        o << ")\n{\n    .reg .s32 %r<" << nextReg + 1 << ">;\n";
        for (auto& l : body) o << l << "\n";
        o << "    ret;\n}\n";
        return o.str();
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { cerr << "usage: minipx input.txt [--no-fold]\n"; return 1; }
    bool doFold = !(argc > 2 && string(argv[2]) == "--no-fold");
    ifstream f(argv[1]);
    if (!f) { cerr << "cannot open " << argv[1] << "\n"; return 1; }
    stringstream ss; ss << f.rdbuf();
    try {
        Parser ps{lex(ss.str())};
        auto prog = ps.program();
        CodeGen cg;
        for (auto& s : prog) {
            if (doFold) s.expr = fold(move(s.expr));
            cg.stmt(s);
        }
        cout << cg.emit();
        cerr << "[minipx] statements: " << prog.size() << ", folds: " << foldCount
             << ", instructions: " << cg.body.size() << "\n";
    } catch (const exception& e) { cerr << e.what() << "\n"; return 1; }
}
