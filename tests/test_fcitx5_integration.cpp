#include "shadowbar/config.hpp"

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <testfrontend_public.h>
#include <testim_public.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef SHADOWBAR_FCITX5_MAJOR
#define SHADOWBAR_FCITX5_MAJOR 5
#define SHADOWBAR_FCITX5_MINOR 0
#define SHADOWBAR_FCITX5_PATCH 0
#endif

#define SHADOWBAR_FCITX5_AT_LEAST(major, minor, patch)                         \
    ((SHADOWBAR_FCITX5_MAJOR > (major)) ||                                     \
     (SHADOWBAR_FCITX5_MAJOR == (major) &&                                     \
      (SHADOWBAR_FCITX5_MINOR > (minor) ||                                     \
       (SHADOWBAR_FCITX5_MINOR == (minor) &&                                   \
        SHADOWBAR_FCITX5_PATCH >= (patch)))))

namespace {

bool sendTestKey(fcitx::AddonInstance *testfrontend,
                 const fcitx::ICUUID &uuid,
                 const fcitx::Key &key,
                 bool expect_accepted) {
#if SHADOWBAR_FCITX5_AT_LEAST(5, 1, 0)
    return testfrontend->call<fcitx::ITestFrontend::sendKeyEvent>(
               uuid, key, false) == expect_accepted;
#else
    (void)expect_accepted;
    testfrontend->call<fcitx::ITestFrontend::keyEvent>(uuid, key, false);
    return true;
#endif
}

std::filesystem::path setupTempConfigHome() {
    auto tmp_base =
        std::filesystem::temp_directory_path() / "shadowbar_fcitx5_test_cfg";
    std::error_code ec;
    std::filesystem::remove_all(tmp_base, ec);
    std::filesystem::create_directories(tmp_base / "shadowbar", ec);

    shadowbar::ShadowConfig cfg = shadowbar::ShadowConfig::makeDefault();
    cfg.prefix_only_in_cjk_im = false; // Allow ";;" on testim in test harness.
    cfg.prefix_timeout_ms = 120;
    cfg.debounce_ms = 0;
    cfg.active_preset = "offline_test";
    cfg.presets = {
        shadowbar::Preset{
            .id = "offline_test",
            .label = "离线测试",
            .provider = shadowbar::ProviderKind::Offline,
            .target_lang = "en",
        },
        shadowbar::Preset{
            .id = "offline_natural",
            .label = "自然离线",
            .provider = shadowbar::ProviderKind::Offline,
            .target_lang = "en",
        },
    };
    std::ofstream out(tmp_base / "shadowbar" / "config.toml");
    out << shadowbar::ConfigManager::serializeToml(cfg);
    out.flush();

    setenv("XDG_CONFIG_HOME", tmp_base.c_str(), 1);
    setenv("SHADOWBAR_NO_SPAWN_UI", "1", 1);
    return tmp_base;
}

} // namespace

int main() {
    const auto cfg_home = setupTempConfigHome();

    fcitx::setupTestingEnvironment(
        SHADOWBAR_TEST_DATA_DIR,
        {SHADOWBAR_BUILD_DIR, SHADOWBAR_FCITX5_ADDON_DIR, "/usr/lib/fcitx5"},
        {SHADOWBAR_TEST_DATA_DIR, SHADOWBAR_FCITX5_DATADIR, "/usr/share/fcitx5"});

    char arg0[] = "test_fcitx5_integration";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,testim,shadowbar,shadowbarim";
    char arg3[] = "-u";
    char arg4[] = "testui";
    char *argv[] = {arg0, arg1, arg2, arg3, arg4};

    fcitx::Instance instance(5, argv);
    instance.addonManager().registerDefaultLoader(nullptr);

    auto exit_timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 100000ULL, 0,
        [&](fcitx::EventSourceTime *, uint64_t) {
            auto *testfrontend = instance.addonManager().addon("testfrontend");
            auto *testim = instance.addonManager().addon("testim");
            auto *shadowbar = instance.addonManager().addon("shadowbar");
            assert(testfrontend != nullptr);
            assert(testim != nullptr);
            assert(shadowbar != nullptr);

            // Configure testim to simulate Pinyin selecting a Chinese word on Space.
            testim->call<fcitx::ITestIM::setHandler>(
                [](const fcitx::InputMethodEntry &, fcitx::KeyEvent &ev) {
                    if (ev.isRelease()) {
                        return;
                    }
                    auto *ic = ev.inputContext();
                    if (ev.key().check(FcitxKey_a)) {
                        ic->inputPanel().setPreedit(fcitx::Text("nihao"));
                        ic->updateUserInterface(
                            fcitx::UserInterfaceComponent::InputPanel);
                        ev.filterAndAccept();
                    } else if (ev.key().check(FcitxKey_space) &&
                               !ic->inputPanel().preedit().empty()) {
                        ic->commitString("你好");
                        ic->inputPanel().reset();
                        ic->updateUserInterface(
                            fcitx::UserInterfaceComponent::InputPanel);
                        ev.filterAndAccept();
                    } else if (ev.key().check(FcitxKey_b)) {
                        ic->commitString("测试");
                        ev.filterAndAccept();
                    }
                });

            auto uuid = testfrontend->call<fcitx::ITestFrontend::createInputContext>(
                "test-app");
            auto *ic = instance.inputContextManager().findByUUID(uuid);
            assert(ic != nullptr);
            ic->focusIn();
            instance.setCurrentInputMethod(ic, "testim", true);

            // Test 1: Sticky toggle via Ctrl+Shift+T + simulated Pinyin commit interception.
            // Note: Check whether CommitFilter clearing orig skips commitStringImpl or emits "".
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key("Control+Shift+T"), true));

            // Type 'a' -> Pinyin preedit starts, ShadowBar shows alongside it.
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_a), true));
            assert(!ic->inputPanel().preedit().empty());
            assert(!ic->inputPanel().auxUp().empty());

            // Press Space -> Pinyin calls commitString("你好"), ShadowBar intercepts it!
            // Fcitx5 CommitFilter clears orig to "", so commitStringImpl("") fires.
            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>("");
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_space),
                               true));
            assert(ic->inputPanel().auxUp().toString().find("你好") !=
                   std::string::npos);
            // Default compact mode: clean 2-line AuxUp + AuxDown with NO expanded action list.
            assert(!ic->inputPanel().candidateList());
            assert(ic->inputPanel().auxDown().toString().find("hello") !=
                   std::string::npos);

            // Pressing Down once transiently expands interactive 5-row action card.
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Down),
                               true));
            {
                auto cand = ic->inputPanel().candidateList();
                assert(cand && cand->size() == 5);
                assert(cand->cursorIndex() == 0);
                assert(cand->candidate(0).text().toString().find("hello") !=
                       std::string::npos);
                assert(cand->candidate(1).text().toString() == "你好");
                assert(cand->label(0).toString().find("↵") != std::string::npos);
            }

            // Down moves highlight to Row 1 (原文), Up brings it back to Row 0,
            // and pressing Up at Row 0 collapses back to compact mode!
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Down),
                               true));
            assert(ic->inputPanel().candidateList()->cursorIndex() == 1);
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Up),
                               true));
            assert(ic->inputPanel().candidateList()->cursorIndex() == 0);
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Up),
                               true));
            assert(!ic->inputPanel().candidateList());
            assert(ic->inputPanel().auxDown().toString().find("hello") !=
                   std::string::npos);

            // Re-expand once to verify Return on highlighted candidate works too.
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Down),
                               true));
            assert(ic->inputPanel().candidateList() &&
                   ic->inputPanel().candidateList()->size() == 5);

            // Press Return -> commits translated English "hello" to TestFrontend!
            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                "hello");
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Return),
                               true));
            assert(ic->inputPanel().auxUp().empty());
            assert(!ic->inputPanel().candidateList());

            // Return on empty Sticky passes through (!filtered -> sendKeyEvent returns false).
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Return),
                               false));
            assert(ic->inputPanel().auxUp().empty());

            // Toggle Sticky back Off.
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key("Control+Shift+T"), true));

            // Test 2: ";;" prefix OneShot + Shift+Enter raw fallback.
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key(FcitxKey_semicolon), true));
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key(FcitxKey_semicolon), true));
            assert(ic->inputPanel().auxUp().toString().find("请输入") !=
                   std::string::npos);
            // Compact idle OneShot: no candidate popup unless user presses Down.
            assert(!ic->inputPanel().candidateList());
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Down),
                               true));
            {
                auto idle_cand = ic->inputPanel().candidateList();
                assert(idle_cand && idle_cand->size() == 3);
                assert(idle_cand->cursorIndex() == 0);
                assert(idle_cand->candidate(2).text().toString().find("切回纯中文") !=
                       std::string::npos);
            }
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Up),
                               true));
            assert(!ic->inputPanel().candidateList());

            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>("");
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_b), true));
            assert(ic->inputPanel().auxUp().toString().find("测试") !=
                   std::string::npos);

            // Alt+L cycles target language live on the panel (中→EN -> 中→日 -> 中→EN).
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key(FcitxKey_l, fcitx::KeyState::Alt),
                               true));
            assert(ic->inputPanel().auxUp().toString().find("中→日") !=
                   std::string::npos);
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key(FcitxKey_l, fcitx::KeyState::Alt),
                               true));
            assert(ic->inputPanel().auxUp().toString().find("中→EN") !=
                   std::string::npos);

            // Expand with Down, then select Row 1 (click / select(ic)) commits raw "测试" and exits OneShot.
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Down),
                               true));
            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                "测试");
            assert(ic->inputPanel().candidateList() &&
                   ic->inputPanel().candidateList()->size() == 5);
            ic->inputPanel().candidateList()->candidate(1).select(ic);
            assert(ic->inputPanel().auxUp().empty());

            // Test 3: Password capability ignores Ctrl+Shift+T and ";;".
            ic->setCapabilityFlags(fcitx::CapabilityFlag::Password);
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key("Control+Shift+T"), false));
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key(FcitxKey_semicolon), false));
            assert(ic->inputPanel().auxUp().empty());
            assert(!ic->inputPanel().candidateList());

            // Test 4: Dedicated "shadowbar" InputMethod loads & auto-enables Sticky mode.
            ic->setCapabilityFlags(fcitx::CapabilityFlags{});
            assert(instance.inputMethodManager().entry("shadowbar") != nullptr);
            {
                fcitx::InputMethodGroup group =
                    instance.inputMethodManager().currentGroup();
                group.inputMethodList().emplace_back("shadowbar");
                instance.inputMethodManager().setGroup(std::move(group));
            }
            instance.setCurrentInputMethod(ic, "shadowbar", true);
            assert(instance.inputMethod(ic) == "shadowbar");
            auto *shadowbarim = instance.addonManager().addon("shadowbarim");
            assert(shadowbarim != nullptr);
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_h), true));
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_i), true));
            assert(ic->inputPanel().auxUp().toString().find("hi") !=
                   std::string::npos);
            assert(ic->inputPanel().auxUp().toString().find("EN→中") !=
                   std::string::npos);
            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                "hi");
            assert(sendTestKey(
                testfrontend, uuid,
                fcitx::Key(FcitxKey_Return, fcitx::KeyState::Shift), true));

            // Toggle off from "shadowbar" IM -> stays in pure-Chinese passthrough even across focus change.
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key("Control+Shift+T"), true));
            ic->focusOut();
            ic->focusIn();
            assert(ic->inputPanel().auxUp().empty());
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_h),
                               false));
            assert(ic->inputPanel().auxUp().empty());
            assert(ic->inputPanel().auxDown().empty());

            // Toggle back on -> resumes Sticky translation bar.
            assert(sendTestKey(testfrontend, uuid,
                               fcitx::Key("Control+Shift+T"), true));
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_h), true));
            assert(ic->inputPanel().auxUp().toString().find("h") !=
                   std::string::npos);
            testfrontend->call<fcitx::ITestFrontend::pushCommitExpectation>(
                "h");
            assert(sendTestKey(
                testfrontend, uuid,
                fcitx::Key(FcitxKey_Return, fcitx::KeyState::Shift), true));

            instance.setCurrentInputMethod(ic, "keyboard-us", true);
            assert(sendTestKey(testfrontend, uuid, fcitx::Key(FcitxKey_Return),
                               false));

            // Test 5: Global Alt+Shift+L & Alt+Shift+P work when ShadowBar is Off,
            // keep panel closed, and persist across other windows and future activations.
            assert(sendTestKey(testfrontend, uuid, fcitx::Key("Alt+Shift+L"),
                               true)); // en -> zh
            assert(sendTestKey(testfrontend, uuid, fcitx::Key("Alt+Shift+L"),
                               true)); // zh -> ja
            assert(sendTestKey(testfrontend, uuid, fcitx::Key("Alt+Shift+P"),
                               true)); // offline_test -> offline_natural
            assert(ic->inputPanel().preedit().empty());
            assert(ic->inputPanel().auxDown().empty());
            assert(!ic->inputPanel().candidateList());
#if SHADOWBAR_FCITX5_AT_LEAST(5, 1, 22)
            assert(ic->inputPanel().auxUp().empty());
#else
            assert(ic->inputPanel().auxUp().toString().find("请输入") ==
                       std::string::npos &&
                   ic->inputPanel().auxUp().toString().find("│") ==
                       std::string::npos);
#endif

            auto uuid2 =
                testfrontend->call<fcitx::ITestFrontend::createInputContext>(
                    "window2");
            auto *ic2 = instance.inputContextManager().findByUUID(uuid2);
            assert(ic2 != nullptr);
            ic2->focusIn();
            instance.setCurrentInputMethod(ic2, "pinyin", true);
            // Trigger ";;" in second window -> remembers 中→日 + 自然离线!
            assert(sendTestKey(testfrontend, uuid2,
                               fcitx::Key(FcitxKey_semicolon), true));
            assert(sendTestKey(testfrontend, uuid2,
                               fcitx::Key(FcitxKey_semicolon), true));
            assert(ic2->inputPanel().auxUp().toString().find("中→日") !=
                   std::string::npos);
            assert(ic2->inputPanel().auxUp().toString().find("自然离线") !=
                   std::string::npos);
            testfrontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid2);

            testfrontend->call<fcitx::ITestFrontend::destroyInputContext>(uuid);
            instance.exit();
            return false;
        });

    const int rc = instance.exec();
    (void)exit_timer;
    std::error_code ec;
    std::filesystem::remove_all(cfg_home, ec);
    assert(rc == 0);
    std::cout << "ALL Fcitx5 integration tests PASSED.\n";
    return 0;
}
