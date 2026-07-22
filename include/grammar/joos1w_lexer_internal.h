#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <memory_resource>
#ifndef INCLUDED_FLEXLEXER_H
   #warning "This file should not be included directly"
   #include <FlexLexer.h>
#endif

#include "diagnostics/Diagnostics.h"
#include "diagnostics/Location.h"
#include "diagnostics/SourceManager.h"
#include "joos1w.parser.tab.h"
#include "parsetree/ParseTree.h"
#include "utils/BumpAllocator.h"

class Joos1WParser;

class Joos1WLexer : public yyFlexLexer {
   using Node = parsetree::Node;
   using Operator = parsetree::Operator;
   using Literal = parsetree::Literal;
   using Identifier = parsetree::Identifier;
   using Modifier = parsetree::Modifier;
   using BasicType = parsetree::BasicType;
   friend class Joos1WParser;

private:
   Joos1WLexer(BumpAllocator& alloc, diagnostics::DiagnosticEngine* diag,
               SourceFile file = {})
         : file{file}, yycolumn{1}, diag{diag}, alloc{alloc}, messages{alloc} {}

public:
   /// @brief This is the generate Flex lexer function
   int yylex();
   // This is a bison-specific lexer function, implemented in the .l file
   int bison_lex(YYSTYPE* lvalp, YYLTYPE* llocp);

   /// @brief Wrapper around the node constructor
   /// @param ...args The arguments to the node constructor
   /// @return The newly created node
   template <typename... Args>
   Node* make_node(YYLTYPE& loc, Node::Type type, Args&&... args) {
      void* bytes = alloc.allocate_bytes(sizeof(Node));
      return new(bytes)
            Node(make_range(loc), alloc, type, std::forward<Args>(args)...);
   }
   /// @brief Wrapper around constructing a dataless leaf node
   /// @param type The type of the leaf node
   /// @return The newly created leaf node
   Node* make_leaf(YYLTYPE& loc, Node::Type type) {
      void* bytes = alloc.allocate_bytes(sizeof(Node));
      return new(bytes) Node(make_range(loc), type);
   }
   /// @brief See make_node
   Node* make_poison(YYLTYPE& loc);
   /// @brief See make_node
   Node* make_operator(YYLTYPE& loc, Operator::Type type);
   /// @brief See make_node
   Node* make_literal(YYLTYPE& loc, Literal::Type type, char const* value);
   /// @brief See make_node
   Node* make_identifier(YYLTYPE& loc, char const* name);
   /// @brief See make_node
   Node* make_modifier(YYLTYPE& loc, Modifier::Type type);
   /// @brief See make_node
   Node* make_basic_type(YYLTYPE& loc, BasicType::Type type);

   /// @brief Report a parser or lexer error to the diagnostic engine
   /// @param loc The location of the error
   /// @param msg The message to report
   /// @param ranges List of additional ranges to report
   inline void report_parser_error(YYLTYPE& loc, char const* msg,
                                   std::initializer_list<YYLTYPE> ranges = {}) {
      if(!diag) return;
      auto os = diag->ReportError(make_range(loc));
      // Copy msg to the heap
      size_t length = std::min(1024UL, strlen(msg) + 1);
      char* ptr = reinterpret_cast<char*>(alloc.allocate(length));
      strncpy(ptr, msg, length);
      messages.push_back(ptr);
      os << std::string_view{ptr};
      for(auto const& range : ranges) os << make_range(range);
   }

private:
   /// @brief This is a private function that is called by the lexer to handle
   /// comments. It is implemented in the .l file
   void comment();

   /// @brief Converts the lexer location to a source range
   SourceRange make_range(YYLTYPE const& loc) {
      return SourceRange{SourceLocation{file, loc.first_line, loc.first_column},
                         SourceLocation{file, loc.last_line, loc.last_column}};
   }

private:
   SourceFile file;
   YYLTYPE yylloc; // This is the LEXER location
   YYSTYPE yylval;
   int yycolumn;
   diagnostics::DiagnosticEngine* diag;
   BumpAllocator& alloc;
   std::pmr::vector<const char*> messages;
};
