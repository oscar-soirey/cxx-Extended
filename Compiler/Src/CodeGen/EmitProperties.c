/*
 * CodeGen/EmitProperties.c
 *
 * Property getters/setters and property access rewriting.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"
#include "Parser/ParserInternal.h"
#include "Sema/TypeLookup.h"

static int property_assign_token(const Token *t) {
    if (!t) return 0;
    return !strcmp(t->text,"=") || !strcmp(t->text,"+=") || !strcmp(t->text,"-=") ||
           !strcmp(t->text,"*=") || !strcmp(t->text,"/=") || !strcmp(t->text,"%=") ||
           !strcmp(t->text,"&=") || !strcmp(t->text,"|=") || !strcmp(t->text,"^=") ||
           !strcmp(t->text,"<<=") || !strcmp(t->text,">>=");
}

static void emit_property_block(const IR *ir, const IRNode *block, const IRNode *prop, int is_setter, Str *out) {
    size_t a=(size_t)block->first_tok,b=(size_t)block->last_tok,i,open=SIZE_MAX;
    const char *name = prop && prop->name ? prop->name : NULL;
    for(i=a;i<=b && i<ir->tokens.n;++i) if(!strcmp(ir->tokens.v[i].text,"{")){open=i;break;}
    if(open==SIZE_MAX || b<=open) return;
    for(i=open+1;i<=b-1;++i){
        const Token *t=&ir->tokens.v[i];
        const Token *prev = i > open + 1 ? &ir->tokens.v[i-1] : NULL;
        const Token *prev2 = i > open + 2 ? &ir->tokens.v[i-2] : NULL;
        const Token *next = i + 1 < b ? &ir->tokens.v[i+1] : NULL;
        str_put(out,t->trivia);
        if(is_setter && t->kind==TK_IDENTIFIER && !strcmp(t->text,"value")) {
            int is_this_member = prev && !strcmp(prev->text,"->") && prev2 && !strcmp(prev2->text,"this");
            int is_field_write = next && (property_assign_token(next) || !strcmp(next->text,"++") || !strcmp(next->text,"--"));
            if(name && !strcmp(name,"value") && !is_this_member && is_field_write) {
                str_put(out,"this->value");
            } else {
                str_put(out,"CXXEPropertyAssignedValue");
            }
            continue;
        }
        if(name && *name && t->kind==TK_IDENTIFIER && !strcmp(t->text,name)){
            str_put(out,"this->");
            str_put(out,name);
        } else {
            str_put(out,t->text);
        }
    }
}

void emit_property(const IR *ir, const IRNode *prop, Str *out) {
    int64_t g=property_child_kind(ir,prop,N_PROPERTY_GET), s=property_child_kind(ir,prop,N_PROPERTY_SET);
    const char *name=prop->name?prop->name:"Property";
    if(g>=0){ str_put(out,"decltype(auto) get_"); str_put(out,name); str_put(out,"() {"); emit_property_block(ir,&ir->nodes[g],prop,0,out); str_put(out,"}\n"); }
    if(s>=0){ str_put(out,"template <typename CXXEPropertyValue>\nvoid set_"); str_put(out,name); str_put(out,"(CXXEPropertyValue&& CXXEPropertyAssignedValue) {"); emit_property_block(ir,&ir->nodes[s],prop,1,out); str_put(out,"}\n"); }
}

int property_access_is_write(const IR *ir, const IRNode *node) {
    size_t i=(size_t)node->last_tok+1;
    if (i>=ir->tokens.n) return 0;
    return !strcmp(ir->tokens.v[i].text,"=") || !strcmp(ir->tokens.v[i].text,"+=") || !strcmp(ir->tokens.v[i].text,"-=") ||
           !strcmp(ir->tokens.v[i].text,"*=") || !strcmp(ir->tokens.v[i].text,"/=") || !strcmp(ir->tokens.v[i].text,"%=") ||
           !strcmp(ir->tokens.v[i].text,"++") || !strcmp(ir->tokens.v[i].text,"--");
}

void emit_property_access(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok;
    size_t sep=SIZE_MAX;
    if(b<a || node->aux>=ir->node_count){ append_token_bytes(ir,out,a,b); return; }
    {
        const IRNode *prop=&ir->nodes[node->aux];
        if(!property_has_get(ir,prop) || property_access_is_write(ir,node)){ append_token_bytes(ir,out,a,b); return; }
    }
    if (b > a && (!strcmp(ir->tokens.v[b-1].text,".") || !strcmp(ir->tokens.v[b-1].text,"->"))) sep=b-1;
    if (sep==SIZE_MAX) { append_token_bytes(ir,out,a,b); return; }
    append_token_bytes(ir,out,a,sep-1);
    str_put(out,ir->tokens.v[sep].text);
    str_put(out,"get_");
    str_put(out,ir->nodes[node->aux].name?ir->nodes[node->aux].name:node->name);
    str_put(out,"()");
}

int node_is_property_assignment(const IR *ir, const IRNode *node, int64_t *prop_id_out, size_t *op_out, size_t *sep_out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok,i;
    int par=0,br=0,cur=0, top_semis=0;
    size_t op=SIZE_MAX, sep=SIZE_MAX; int64_t pid=-1;
    for(i=a;i<=b;++i){
        const char*t=ir->tokens.v[i].text;
        if(!strcmp(t,"("))par++; else if(!strcmp(t,")")&&par)par--; else if(!strcmp(t,"["))br++; else if(!strcmp(t,"]")&&br)br--; else if(!strcmp(t,"{"))cur++; else if(!strcmp(t,"}")&&cur)cur--;
        else if(!par&&!br&&!cur&&!strcmp(t,";")) top_semis++;
        else if(!par&&!br&&!cur&&op==SIZE_MAX&&i>a&&ir->tokens.v[i-1].kind==TK_IDENTIFIER &&
                ( !strcmp(t,"=") || !strcmp(t,"+=") || !strcmp(t,"-=") || !strcmp(t,"*=") ||
                  !strcmp(t,"/=") || !strcmp(t,"%=") || !strcmp(t,"&=") || !strcmp(t,"|=") ||
                  !strcmp(t,"^=") || !strcmp(t,"<<=") || !strcmp(t,">>="))){
            size_t pn=i-1;
            if(pn>a && (!strcmp(ir->tokens.v[pn-1].text,".")||!strcmp(ir->tokens.v[pn-1].text,"->")) &&
               resolve_property_for_member_ir(ir, pn - 1, ir->tokens.v[pn - 2].text, ir->tokens.v[pn].text, &pid)){
                op=i; sep=pn-1;
            }
        }
    }
    if(top_semis>1 || op==SIZE_MAX) return 0;
    if(prop_id_out)*prop_id_out=pid;
    if(op_out)*op_out=op;
    if(sep_out)*sep_out=sep;
    return 1;
}

void emit_property_unary(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok;
    const IRNode *prop;
    const char *op;
    size_t sep=SIZE_MAX;
    int prefix;
    if (node->aux >= ir->node_count) { append_token_bytes(ir,out,a,b); return; }
    prop=&ir->nodes[node->aux];
    op=node->name ? node->name : ((a < ir->tokens.n) ? ir->tokens.v[a].text : "");
    prefix = a < ir->tokens.n && (!strcmp(ir->tokens.v[a].text,"++") || !strcmp(ir->tokens.v[a].text,"--"));
    if (prefix) {
        if (b < a + 3) { append_token_bytes(ir,out,a,b); return; }
        sep = a + 2;
        if (sep >= ir->tokens.n || (strcmp(ir->tokens.v[sep].text,".") && strcmp(ir->tokens.v[sep].text,"->"))) { append_token_bytes(ir,out,a,b); return; }
        if (!property_has_get(ir,prop) || !property_has_set(ir,prop))
            die("CXXE: property '%s' needs both getter and setter for ++/--",prop->name?prop->name:"<unnamed>");
        str_put(out,"([&]() { ");
        append_token_bytes(ir,out,a+1,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"set_"); str_put(out,prop->name); str_put(out,"(");
        append_token_bytes(ir,out,a+1,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"get_"); str_put(out,prop->name); str_put(out,"() ");
        str_put(out,!strcmp(op,"++")?"+ 1":"- 1");
        str_put(out,"); return ");
        append_token_bytes(ir,out,a+1,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"get_"); str_put(out,prop->name); str_put(out,"(); }())");
    } else {
        if (b < a + 3) { append_token_bytes(ir,out,a,b); return; }
        sep = b - 2;
        if (sep >= ir->tokens.n || (strcmp(ir->tokens.v[sep].text,".") && strcmp(ir->tokens.v[sep].text,"->"))) { append_token_bytes(ir,out,a,b); return; }
        if (!property_has_get(ir,prop) || !property_has_set(ir,prop))
            die("CXXE: property '%s' needs both getter and setter for ++/--",prop->name?prop->name:"<unnamed>");
        str_put(out,"([&]() { auto __cxxe_old = ");
        append_token_bytes(ir,out,a,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"get_"); str_put(out,prop->name); str_put(out,"(); ");
        append_token_bytes(ir,out,a,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"set_"); str_put(out,prop->name); str_put(out,"(__cxxe_old ");
        str_put(out,!strcmp(op,"++")?"+ 1":"- 1"); str_put(out,"); return __cxxe_old; }())");
        if (b > a && !strcmp(ir->tokens.v[b].text,";")) str_ch(out,';');
    }
}

void emit_property_assignment(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok,op=SIZE_MAX,sep=SIZE_MAX;
    int64_t pid=-1;
    const char *op_text;
    if(!node_is_property_assignment(ir,node,&pid,&op,&sep)){ append_token_bytes(ir,out,a,b); return; }
    if(!property_has_set(ir,&ir->nodes[pid])) die("CXXE: property '%s' has no setter",ir->nodes[pid].name?ir->nodes[pid].name:"<unnamed>");
    op_text=ir->tokens.v[op].text;
    append_token_bytes(ir,out,a,sep-1);
    str_put(out,ir->tokens.v[sep].text);
    str_put(out,"set_"); str_put(out,ir->nodes[pid].name?ir->nodes[pid].name:"Property"); str_ch(out,'(');
    if(strcmp(op_text,"=")!=0){
        size_t n;
        if(!property_has_get(ir,&ir->nodes[pid])) die("CXXE: property '%s' needs a getter for compound assignment '%s'",ir->nodes[pid].name?ir->nodes[pid].name:"<unnamed>",op_text);
        append_token_bytes(ir,out,a,sep-1);
        str_put(out,ir->tokens.v[sep].text); str_put(out,"get_"); str_put(out,ir->nodes[pid].name?ir->nodes[pid].name:"Property"); str_put(out,"()");
        n=strlen(op_text); if(n && op_text[n-1]=='=') n--;
        str_put(out," "); if(n) str_putn(out,op_text,n); str_put(out," ");
    }
    if(op+1<=b){
        size_t rhs_b=b;
        if(rhs_b>=a && !strcmp(ir->tokens.v[rhs_b].text,";")) rhs_b--;
        if(op+1<=rhs_b) emit_native_token_range(ir,out,op+1,rhs_b);
    }
    str_ch(out,')');
    if(b>=a && !strcmp(ir->tokens.v[b].text,";")) str_ch(out,';');
}
