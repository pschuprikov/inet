#pragma once

#include <iterator>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

class CSVRow
{
    public:
      explicit CSVRow(char csv_sep);

      std::string operator[](std::size_t index) const;
      std::size_t size() const;
      void readNextRow(std::istream& str);
    private:
      char                m_csv_sep;
      std::string         m_line;
      std::vector<int>    m_data;
};

std::istream& operator>>(std::istream& str, CSVRow& data);