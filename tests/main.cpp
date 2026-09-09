#include <cstdio>

#include "check.hpp"

void TestTalk();
void TestPeople();
void TestField();
void TestLocal();

int main() {
  std::printf("gtabot tests\n\n");
  TestTalk();
  TestPeople();
  TestField();
  TestLocal();
  std::printf("\n%d checks, %d failed\n", check::g_ran, check::g_failed);
  return check::g_failed == 0 ? 0 : 1;
}
