#include <cstdio>

#include "check.hpp"

void TestTalk();
void TestPeople();
void TestField();

int main() {
  std::printf("gtabot tests\n\n");
  TestTalk();
  TestPeople();
  TestField();
  std::printf("\n%d checks, %d failed\n", check::g_ran, check::g_failed);
  return check::g_failed == 0 ? 0 : 1;
}
