#include <fstream>
#include <gtest/gtest.h>

#include "solc/driver/Driver.h"

TEST(Smoke, DriverRejectsMissingFile) {
    solc::CompileOptions opts;
    opts.inputPath = "/no/such/file.sol";
    solc::Driver drv;
    EXPECT_NE(drv.run(opts), 0);
}

TEST(Smoke, DriverAcceptsExistingFile) {
    const char* path = "/tmp/solc_smoke.sol";
    {
        std::ofstream f(path);
        f << "contract X {}";
    }
    solc::CompileOptions opts;
    opts.inputPath = path;
    opts.emitIR    = true;
    opts.legacy    = true;   // юнит-тест драйвера без зависимости от docker/solc
    solc::Driver drv;
    EXPECT_EQ(drv.run(opts), 0);
}

TEST(Smoke, DriverFailsOnUnwritableOutput) {
    const char* in = "/tmp/solc_smoke_in.sol";
    {
        std::ofstream f(in);
        f << "contract X {}";
    }
    solc::CompileOptions opts;
    opts.inputPath  = in;
    opts.outputPath = "/no/such/dir/out.ll";
    opts.emitIR     = true;
    opts.legacy     = true;   // юнит-тест драйвера без зависимости от docker/solc

    solc::Driver drv;
    EXPECT_NE(drv.run(opts), 0);
}
