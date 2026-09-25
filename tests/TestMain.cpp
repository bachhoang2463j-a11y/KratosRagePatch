// ---------------------------------------------------------------------------
// 测试入口：顺序执行注册用例 + fixture 驱动用例，汇总失败并以退出码反馈。
// 用法：KratosRageTests.exe [fixture目录]（默认自动定位 tests/fixtures）。
//
// 说明：名字含 spec_gap 的 fixture 用例是"按契约编写、用于暴露 src 缺陷"的用例，
//       若它们失败，说明 RageState 的实现与契约不符，而不是测试写错。
// ---------------------------------------------------------------------------

#include "TestSupport.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::string explicitDir = (argc > 1) ? std::string(argv[1]) : std::string();
    const std::string fixtureDir = krtest::ResolveFixtureDir(explicitDir, __FILE__);

    std::cout << "== KratosRagePatch offline tests ==\n";
    std::cout << "fixture dir: " << fixtureDir << "\n\n";

    int failedCases = 0;
    int totalCases = 0;

    for (const krtest::TestCase& test : krtest::Registry()) {
        ++totalCases;
        const std::size_t before = krtest::Failures().size();
        krtest::SetCurrentCase(test.name);
        test.fn();
        const std::size_t failures = krtest::Failures().size() - before;
        if (failures == 0) {
            std::cout << "[PASS] " << test.name << "\n";
        } else {
            ++failedCases;
            std::cout << "[FAIL] " << test.name << " (" << failures << " failed checks)\n";
        }
    }

    std::cout << "\n-- fixture cases --\n";
    const int hitFixtureFailures = krtest::RunHitFixtureCases(fixtureDir);
    const int activationFixtureFailures = krtest::RunActivationFixtureCases(fixtureDir);

    if (!krtest::Failures().empty()) {
        std::cout << "\n-- failures (" << krtest::Failures().size() << ") --\n";
        for (const krtest::Failure& failure : krtest::Failures()) {
            std::cout << failure.where << ": " << failure.message << "\n";
        }
    }

    std::cout << "\nsummary: registered tests " << totalCases
              << ", registered failures " << failedCases
              << ", fixture failures " << (hitFixtureFailures + activationFixtureFailures)
              << ", checks " << krtest::Checks()
              << ", assertion failures " << krtest::Failures().size() << "\n";
    std::cout << "note: [SPEC-GAP-EXPECTED-FAIL] entries are contract cases expected to fail"
                 " against the current src implementation.\n";

    return krtest::Failures().empty() ? 0 : 1;
}
