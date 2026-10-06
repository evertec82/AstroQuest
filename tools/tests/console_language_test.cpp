// Tries Common::ConsoleLanguageFromTag (shadps4-arm64-main/src/common/console_language.h): the
// console's number for a language as Windows, Android and the settings name it.
//   source tools/env-win.sh
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src \
//       tools/tests/console_language_test.cpp /Fe:build/console_language_test.exe
#include <cstdio>
#include <optional>
#include <string>

#include "common/console_language.h"

static int failed = 0;

static void Check(const char* tag, std::optional<int> want) {
    const std::optional<int> got = Common::ConsoleLanguageFromTag(tag);
    if (got == want) {
        std::printf("ok    \"%s\" -> %s\n", tag,
                    got ? Common::ConsoleLanguageName(*got) : "nothing");
    } else {
        ++failed;
        std::printf("FAIL  \"%s\": got %d, want %d\n", tag, got.value_or(-1), want.value_or(-1));
    }
}

int main() {
    // What Windows calls its display languages.
    Check("en-US", 1);
    Check("en-GB", 18);
    Check("en-AU", 18);
    Check("en-CA", 1);
    Check("fr-FR", 2);
    Check("fr-BE", 2);
    Check("fr-CA", 22);
    Check("es-ES", 3);
    Check("es-MX", 20);
    Check("es-419", 20);
    Check("de-DE", 4);
    Check("de-AT", 4);
    Check("it-IT", 5);
    Check("nl-NL", 6);
    Check("pt-PT", 7);
    Check("pt-BR", 17);
    Check("ru-RU", 8);
    Check("ko-KR", 9);
    Check("zh-TW", 10);
    Check("zh-HK", 10);
    Check("zh-Hant", 10);
    Check("zh-CN", 11);
    Check("zh-Hans", 11);
    Check("fi-FI", 12);
    Check("sv-SE", 13);
    Check("da-DK", 14);
    Check("nb-NO", 15);
    Check("nn-NO", 15);
    Check("pl-PL", 16);
    Check("tr-TR", 19);
    Check("ar-SA", 21);
    Check("cs-CZ", 23);
    Check("hu-HU", 24);
    Check("el-GR", 25);
    Check("ro-RO", 26);
    Check("th-TH", 27);
    Check("ja-JP", 0);
    // Android's, and a Unix locale.
    Check("zh-Hans-CN", 11);
    Check("zh-Hant-TW", 10);
    Check("pt_BR", 17);
    Check("fr_FR.UTF-8", 2);
    Check("in-ID", 29);
    Check("vi-VN", 28);
    // A language alone, in any case, with spaces around.
    Check("fr", 2);
    Check("EN", 1);
    Check("es", 3);
    Check("pt", 7);
    Check("zh", 11);
    Check(" de-DE ", 4);
    // The console's own numbers.
    Check("0", 0);
    Check("2", 2);
    Check("29", 29);
    Check("30", std::nullopt);
    Check("123", std::nullopt);
    // What the console has no language for.
    Check("uk-UA", std::nullopt);
    Check("he-IL", std::nullopt);
    Check("", std::nullopt);
    Check("windows", std::nullopt);
    std::printf("failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
