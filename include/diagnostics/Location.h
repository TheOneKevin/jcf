#pragma once

#include <utils/Assert.h>

#include <iostream>
#include <sstream>

#include "diagnostics/SourceManager.h"

class SourceLocation;
class SourceRange;

/* ===--------------------------------------------------------------------=== */
// SourceLocation
/* ===--------------------------------------------------------------------=== */

/// @brief A specific location (line, column) in a source file.
class SourceLocation {
   friend class SourceRange;
   SourceLocation() : file_{}, line_{-1}, column_{-1} {}

public:
   SourceLocation(SourceFile file, int line, int column)
         : file_{file}, line_{line}, column_{column} {}
   std::ostream& print(std::ostream& os) const {
      SourceManager::print(os, file_);
      os << ":" << line_ << ":" << column_;
      return os;
   }
   std::string toString() const {
      std::ostringstream os;
      print(os);
      return os.str();
   }

   /// @brief Returns true if the SourceLocation was not default constructed.
   bool isValid() const { return line_ != -1; }

   SourceFile file() const { return file_; }
   int line() const { return line_; }
   int column() const { return column_; }

private:
   SourceFile file_;
   int line_;
   int column_;
};

/* ===--------------------------------------------------------------------=== */
// SourceRange
/* ===--------------------------------------------------------------------=== */

/// @brief A range of locations in a source file.
class SourceRange {
public:
   /// @brief Construct a new SourceRange with no associated file.
   SourceRange() : begin_{}, end_{} {}
   SourceRange(SourceFile file) : begin_{file, 0, 0}, end_{file, 0, 0} {}

   /// @brief Construct a new SourceRange with the given begin and end
   /// locations.
   SourceRange(SourceLocation begin, SourceLocation end)
         : begin_{begin}, end_{end} {
      assert(begin.file_ == end.file_ && "SourceRange spans multiple files");
   }

   /// @brief Returns true if the SourceRange was not default constructed.
   /// Checks if each SourceLocation is valid.
   bool isValid() const { return begin_.isValid() && end_.isValid(); }

   std::ostream& print(std::ostream& os) const {
      begin_.print(os);
      os << " - " << end_.line_ << ":" << end_.column_;
      return os;
   }

   std::string toString() const {
      std::ostringstream os;
      print(os);
      return os.str();
   }

   SourceLocation range_start() const { return begin_; }
   SourceLocation range_end() const { return end_; }

   static SourceRange merge(SourceRange const& a, SourceRange const& b) {
      assert(a.begin_.file_ == b.begin_.file_ &&
             "Tried to merge SourceRanges from different files");
      auto file = a.begin_.file_;
      if(!a.isValid()) return b;
      if(!b.isValid()) return a;
      return SourceRange{
            // Grab the smallest line & col
            SourceLocation{file,
                           std::min(a.begin_.line_, b.begin_.line_),
                           std::min(a.begin_.column_, b.begin_.column_)},
            // Grab the largest line & col
            SourceLocation{file,
                           std::max(a.end_.line_, b.end_.line_),
                           std::max(a.end_.column_, b.end_.column_)}};
   }

private:
   SourceLocation begin_;
   SourceLocation end_;
};
