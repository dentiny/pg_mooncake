#include "duckdb.hpp"

#include <iostream>

int main(int argc, char** argv) {
  duckdb::DuckDB db(nullptr); // nullptr initializes an in-memory database
  duckdb::Connection con(db);

  con.Query("CREATE TABLE my_table (id INTEGER);");
  std::cout << "Table created successfully.\n";

  // Step 3: Insert an integer value into the table
  con.Query("INSERT INTO my_table VALUES (42);");
  std::cout << "Value inserted successfully.\n";

  // Step 4: Query the table to verify the inserted value
  auto result = con.Query("SELECT * FROM my_table;");
  std::cerr << result->ToString() << std::endl;
  return 0;
}
