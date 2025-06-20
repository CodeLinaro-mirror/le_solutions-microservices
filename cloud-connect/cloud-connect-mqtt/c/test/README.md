## This README defines how to build tests with and without test coverage

- Only to build the test build  
make

- Run the test   
./run_tests

- Clean all the object and binary files   
make clean

- Build the test binary with test coverage enabled   
make COVERAGE=1

- Above command will only generate gcno files, to generate the gcda files Run the test   
./run_tests

- Once the gcda files are generated give below two commands   
  - lcov --capture --directory ./../ --output-file coverage.info
  - genhtml coverage.info --output-directory coverage_report/

- Coverage report in the form of html file will be generated inside coverage_report/ directory.   
