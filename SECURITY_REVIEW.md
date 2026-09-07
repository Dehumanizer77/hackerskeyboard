# Bezpečnostná revízia Hacker's Keyboard

Dátum: 7. 9. 2026. Revidovaný commit: `9202d9d8b1d379f1a20edf08ed7e7149038c8e43`.

## Záver a rozsah

Našiel som **dve vysoko závažné chyby bezpečnosti pamäte**, **dva stredne závažné problémy** a **nedostatok zostavovacieho procesu**. Natívny parser slovníkov odporúčam opraviť pred ďalším vydaním. Chyby čítania a prekročenia polí sú potvrdené lokálnymi testami pôvodného C++ kódu s AddressSanitizerom a UndefinedBehaviorSanitizerom. Spustenie cudzieho kódu ani únik údajov som nepreukázal.

Rozsah: manifest, Java kód klávesnice, JNI a C++ parser, objavovanie slovníkových pluginov, učenie slov, úložiská, notifikácie, zálohovanie, Gradle/CMake a základná kontrola obsahu pribaleného `voiceimeutils.jar` vrátane vybraného bytecode. Nejde o úplný audit tejto binárnej závislosti ani histórie Gitu.

Existujúce nesledované `SECURITY-REVIEW.md`, PDF, `review-tools/` a ostatné používateľské súbory som nemenil a starší report som nepoužil ako dôkaz. Obsah lokálneho `pat.txt` som nečítal ani nevypisoval. Aplikačný kód zostal nezmenený.

Celý Android build ani testy na zariadení neprebehli: `bash gradlew --offline --version` končí `Could not find or load main class org.gradle.wrapper.GradleWrapperMain`; adresár `gradle/` nie je súčasťou checkoutu. Hostiteľské testy parsera nevyžadovali Android SDK ani sieťové závislosti. Závažnosť je kvalitatívna, bez nepodloženého CVSS skóre.

| ID | Závažnosť | Zistenie | Dôkaz |
|---|---|---|---|
| SR-01 | Vysoká | Zápisy mimo polí pri prechádzaní slovníka | Dva UBSan testy |
| SR-02 | Vysoká | Čítanie mimo vstupného slovníka | Dva ASan testy |
| SR-03 | Stredná | Ignorovanie zákazu personalizovaného učenia | Sledovanie toku dát v kóde |
| SR-04 | Stredná | Nechránené notifikačné broadcasty | Kontrola registrácie a prijímača |
| SR-05 | Nízka / procesná | Nereprodukovateľný build a vypnutý release lint | Konfigurácia a neúspešné spustenie wrappera |

## Model hrozieb

Klávesnica spracúva text z viacerých aplikácií. Poškodenie jej procesu je preto citlivejšie než pád bežného prehliadača statických dát. Útočníkom pri SR-01 a SR-02 môže byť autor nainštalovaného slovníkového balíka, ktorý deklaruje podporovaný intent a poskytne podvrhnuté resources. Podmienkou je, že sa jeho slovník skutočne vyberie a načíta. `Suggest.java:110–118` používa plugin ako náhradu, keď hlavný slovník nie je dostatočný; samotná inštalácia ľubovoľnej aplikácie preto neznamená automatické zneužitie vo všetkých jazykoch.

`PluginManager.java:50–116,125–168,191–202,282–300` vyhľadáva cudzie balíky a načítava ich dáta bez overenia podpisu alebo explicitného schválenia konkrétneho poskytovateľa. Ide o import dát, nie priamo o načítanie cudzieho Java kódu. Bezpečnostnú hranicu prekračuje až nebezpečné spracovanie týchto dát v procese IME.

## SR-01 — Zápisy mimo polí v natívnom parseri

**Miesta:** `app/src/main/cpp/dictionary.cpp:283–370,435–550`, najmä riadky 320 a 454; `dictionary.h:107` (`mWord[128]`); `BinaryDictionary.java:40–57,209–233`.

`getWordsRec()` obmedzuje hĺbku podľa trojnásobku dĺžky vstupu, ale nekontroluje kapacitu `mWord`. Java pripúšťa 47 vstupných znakov, takže maximálna povolená hĺbka je 141, hoci pole má iba 128 prvkov. Slovník s dostatočne hlbokou vetvou dosiahne zápis `mWord[128]`.

`searchForTerminalNode()` vytvára lokálne pole `word[mMaxWordLength]`, ale slučka zvyšuje `depth` bez limitu alebo spoľahlivého ukončenia pri nenájdení cieľa. Neplatný bigramový odkaz dokáže opakovane prechádzať tú istú vetvu a zapisovať za 48-prvkové pole. Ani vkladanie výsledku nesmie predpokladať, že dĺžka nájdeného slova automaticky sedí do cieľového slotu.

**Overenie:** test `deep` s parametrami z Java vrstvy hlási `dictionary.cpp:320:24: runtime error: index 128 out of bounds for type 'short unsigned int [128]'`. Test `bigram` hlási `dictionary.cpp:454:23: runtime error: index 48 out of bounds for type 'short unsigned int [*]'`. Oba volajú verejné metódy pôvodného parsera.

**Dopad:** poškodenie pamäte a pád klávesnice. Pri zápise mimo lokálneho poľa existuje potenciál závažnejšej kompromitácie procesu; riadené spustenie kódu na Androide nebolo testované ani preukázané.

**Riešenie:** kontrolovať hĺbku pred každým zápisom, vrátane miesta na ukončovaciu nulu; vynútiť limit `min(kapacita pracovného poľa, maxWordLength) - 1`. Odmietnuť cykly, neplatné odkazy a prechody bez pokroku, zaviesť rozpočet počtu uzlov. Pred kopírovaním kontrolovať dĺžku voči veľkosti cieľového slotu. Kontroly patria aj do natívnej vrstvy, nie iba do Java volajúceho. Ako dočasnú mitigáciu vypnúť import nedôveryhodných slovníkov; samotný allowlist podpisov nenahrádza bezpečný parser.

**Akceptačný test:** oba vstupy odmietnuť bez pádu, zápisu mimo polí alebo nekonečnej slučky; následne fuzzovať návrhy aj bigramy s ASan/UBSan, vrátane dlhých slov a cyklických odkazov.

## SR-02 — Čítanie za koncom slovníkových dát

**Miesta:** `app/src/main/cpp/dictionary.cpp:99–162,378–396`; inline čítačky v `dictionary.h:66–69`; `org_pocketworkstation_pckeyboard_BinaryDictionary.cpp:45–56`; `BinaryDictionary.java:125–148`.

Konštruktor vždy prečíta dvojbajtovú hlavičku bez kontroly minimálnej dĺžky. `getChar()` overí iba prvý bajt, ale pri značke `0xff` číta ďalšie dva bez kontroly. Podobný problém majú viacbajtové adresy a preskakovanie bigramov. Niektoré inline čítačky nekontrolujú hranice vôbec. JNI kontroluje adresu direct buffera, ale neporovnáva deklarovanú veľkosť s jeho kapacitou.

**Overenie:** jednobajtový vstup `c8` spustí ASan `heap-buffer-overflow`, READ size 1 v `getVersionNumber()` na riadku 102. Vstup `c8 00 01 ff` vyvolá ASan `heap-buffer-overflow` v `getChar()` na riadku 120. Nejde iba o hypotetický nedostatok validácie.

**Dopad:** pád pri načítaní alebo používaní slovníka; potenciálne spracovanie susednej pamäte ako dát. Konkrétny únik obsahu pamäte nebol preukázaný.

**Riešenie:** zaviesť spoločnú čítačku s kontrolou celého rozsahu každého čítania (`n <= size - pos` po kontrole `pos`), kontrolovať hlavičku a podporovanú verziu, adresy a ukončenie bigramových zoznamov. Pri chybe ukončiť spracovanie a slovník odmietnuť. V JNI overovať kapacitu cez `GetDirectBufferCapacity`. V Java loaderi nahradiť predpoklad `available() == celková veľkosť` riadnym čítaním do EOF s pevným limitom veľkosti a bezpečným sčítaním veľkostí častí.

**Akceptačný test:** oba uvedené vstupy a všetky skrátené prefixy platného slovníka musia skončiť kontrolovanou chybou bez ASan nálezu. Zahrnúť skrátené adresy a neukončené bigramové zoznamy.

## SR-03 — Učenie aj pri požiadavke na súkromný vstup

**Miesta:** `LatinIME.java:785–908,2792–2833,3325–3328`; `AutoDictionary.java:141–160,167–176,244`; `UserBigramDictionary.java:166–190,370–377`; `UserDictionary.java:104`, všetko pod `app/src/main/java/org/pocketworkstation/pckeyboard/`.

Kód vôbec nekontroluje `EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING`. Pri bežnom textovom poli s týmto príznakom, zapnutými korekciami a funkčným slovníkom zostáva cesta k `checkAddToDictionary()` aktívna. Slová a dvojice slov sa môžu uložiť do `auto_dict.db` a `userbigram_dict.db`; opakované slová sa môžu povýšiť do systémového používateľského slovníka.

**Dopad:** text, pri ktorom aplikácia výslovne požaduje neučenie, môže pretrvať a neskôr sa objaviť v návrhoch. Nie je to dôkaz automatického ukladania všetkých hesiel: textové heslové variácie majú samostatné vypínanie predikcie. Chýbajúci príznak je potvrdený staticky; databázový scenár na zariadení zostáva neotestovaný.

**Riešenie:** vytvoriť politiku učenia pre aktuálny editor a kontrolovať ju na všetkých miestach zápisu, vrátane bigramov a propagácie slov. Pri `IME_FLAG_NO_PERSONALIZED_LEARNING` zakázať personalizované učenie; zahrnúť textové, webové aj numerické heslá a používateľský súkromný režim. Pri zmene editora správne resetovať stav a nepreniesť rozpracovaný citlivý text do nasledujúcej relácie.

**Akceptačný test:** v testovacej aplikácii vytvoriť textové pole s uvedeným príznakom a povolenými korekciami, opakovane napísať unikátne slovo a dvojicu slov, ukončiť vstup, reštartovať IME a overiť, že nepribudli v oboch DB, systémovom slovníku ani návrhoch. Kontrolný editor bez príznaku má učenie zachovať.

Oficiálna definícia príznaku požaduje neaktualizovať personalizované dáta z daného editora: [Android EditorInfo](https://developer.android.com/reference/android/view/inputmethod/EditorInfo#IME_FLAG_NO_PERSONALIZED_LEARNING).

## SR-04 — Cudzie aplikácie môžu volať notifikačný prijímač

**Miesta:** `LatinIME.java:477–500`; `NotificationReceiver.java:10–34`.

Dynamický receiver pre `org.pocketworkstation.pckeyboard.SHOW` a `.SETTINGS` je registrovaný bez permission alebo obmedzenia na vlastnú aplikáciu. Keď je notifikácia zapnutá a receiver žije, iná lokálna aplikácia môže poslať rovnaký broadcast. Handler sa pokúsi vynútiť zobrazenie IME alebo spustiť nastavenia.

Vetva SETTINGS navyše používa `startActivity()` zo servisného kontextu bez `FLAG_ACTIVITY_NEW_TASK`, čo na bežnej Android implementácii vedie k výnimke. Konkrétny pád a správanie pri obmedzeniach novších Android verzií treba potvrdiť na zariadení. Oba notifikačné `PendingIntent` sú implicitné a vytvorené s flags `0`; tým sa zbytočne rozširuje priestor na zneužitie alebo zachytenie akcie. Nie je preukázané čítanie klávesov cez tento receiver.

**Dopad:** lokálne narušenie UI a možná nedostupnosť procesu klávesnice. Podmienkou je aktívny notifikačný receiver; nejde o sieťový útok.

**Riešenie:** interný receiver registrovať ako neexportovaný cez kompatibilné API, prípadne na starších verziách použiť permission úrovne `signature`. Zámer posielania obmedziť na vlastný balík, `PendingIntent` označiť immutable tam, kde to API podporuje. Pre nastavenia použiť priamo explicitný `PendingIntent.getActivity()`. Pri statickom neexportovanom prijímači možno použiť explicitný component, ale treba upraviť súčasný návrh závislý od inštancie IME.

**Akceptačný test:** druhá testovacia aplikácia odošle obe akcie počas aktívnej notifikácie. Po oprave nesmú spustiť handler; vlastná notifikácia musí naďalej fungovať. Testovať aj zatvorenie a opätovné otvorenie IME.

Mechanizmus exportovaných dynamických receiverov a odporúčané obmedzenia opisuje [Android: Insecure broadcast receivers](https://developer.android.com/privacy-and-security/risks/insecure-broadcast-receiver).

## SR-05 — Nedostatky zostavovania a bezpečnostných kontrol

**Miesta:** `app/build.gradle:4–9,31–46`; `build.gradle:7–24`; `.gitignore:13`; chýbajúci `gradle/wrapper/`.

Projekt deklaruje compile/target SDK 26, Android Gradle Plugin 3.2.1, staré support knižnice a JCenter. Release lint je explicitne vypnutý cez `checkReleaseBuilds false`. Wrapper skript je prítomný, jeho podporné súbory však chýbajú a celý adresár `gradle` ignoruje Git. Čistý checkout preto nemožno zostaviť cez dodaný wrapper. V `.github` som našiel iba šablónu issue, žiadny kontrolný workflow.

**Dopad:** sťažené overovanie a bezpečné vydávanie opráv. Samotný vek verzií nie je dôkaz konkrétnej zneužiteľnej CVE; audit transitívnych závislostí sa bez vyriešeného buildu neuskutočnil.

**Riešenie:** doplniť wrapper JAR a properties z overeného zdroja s checksumom distribúcie, upraviť `.gitignore`, zdokumentovať SDK/NDK/JDK. Migrovať na udržiavaný kompatibilný toolchain a repozitáre závislostí, obnoviť release lint a pridať CI pre build, lint a sanitizerové testy parsera. Pri zvýšení target SDK upraviť export komponentov, receiverov a PendingIntent podľa príslušnej platformy. Pre pribalený JAR zaznamenať pôvod, verziu a hash alebo ho nahradiť auditovateľným zdrojom.

**Akceptačný test:** čistý checkout sa zostaví zdokumentovaným príkazom a CI zastaví release pri bezpečnostnej chybe lint alebo sanitizéra.

## Ďalšie pozorovania a hranice záverov

- Hlavný manifest nemá oprávnenie `INTERNET`, `READ_CONTACTS` ani `RECORD_AUDIO`; IME služba je chránená `BIND_INPUT_METHOD`. V preverovanom Java kóde som nenašiel vlastného sieťového klienta. To nie je dôkaz, že hlasový poskytovateľ alebo výsledný zlúčený manifest nemôže využívať sieť.
- `TextEntryState.LOGGING`, `PointerTracker.DEBUG` a `DEBUG_MOVE` sú vypnuté. Prítomnosť diagnostického kódu preto nehodnotím ako potvrdený aktívny keylogger.
- `allowBackup=true` samo osebe nedokazuje zálohovanie naučených slov. `LatinIMEBackupAgent.java:31–33` registruje zálohu shared preferences. Rozsah cloudového zálohovania a prenosu medzi zariadeniami treba overiť na výslednom APK; odporúčam explicitné vylúčenie citlivých slovníkov a dokumentovanú politiku retencie.
- Heslové polia si zaslúžia samostatný regresný test: `shouldShowVoiceButton()` v `LatinIME.java:915–918` vracia vždy `true` a náhľady klávesov sa zapínajú podľa globálnej preferencie. Je to odporúčanie na sprísnenie súkromia, nie preukázané automatické odosielanie hesiel.
- Exportované obrazovky nastavení bez doloženého nebezpečného spracovania extras neoznačujem za samostatnú závažnú zraniteľnosť. Explicitné `android:exported` je vhodné doplniť podľa skutočného použitia.

## Poradie opráv

1. **Pred vydaním:** SR-01 a SR-02, dočasné obmedzenie nedôveryhodných pluginov a sanitizerové regresie.
2. **Nasledujúca oprava súkromia a IPC:** SR-03 a SR-04, integračné testy na Androide.
3. **Súbežne s prípravou vydania:** reprodukovateľný build, aktualizácia závislostí a CI podľa SR-05; následne revízia výsledného APK, hlasovej integrácie a záloh.

## Reprodukcia natívnych testov

Testy boli kompilované pomocou `g++ -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`, s pôvodnými `dictionary.cpp` a `char_utils.cpp`. Použité prostredie: `ASAN_OPTIONS=detect_leaks=0`, `UBSAN_OPTIONS=halt_on_error=1`. Štyri testy skončili kódom 1 s vyššie uvedenými diagnostikami; ide o očakávané zlyhanie zraniteľného parsera, nie o úspešné regresné testy opravy.

Lokálny harness a úplné logy sú v `/tmp/hk-security-review-20260907/`; tento dočasný adresár nie je súčasťou repozitára. Pre opakovanie je zdroj harnessu priložený nižšie. Každý režim spustiť ako samostatný proces.

```bash
g++ -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I app/src/main/cpp probe.cpp app/src/main/cpp/dictionary.cpp \
  app/src/main/cpp/char_utils.cpp -o probe
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./probe header
# Zopakovať s argumentmi: truncated, bigram, deep.
```

```cpp
#include "dictionary.h"
#include <vector>
#include <string>
int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "header";
    std::vector<unsigned char> data;
    if (mode == "header") data = {200};
    else if (mode == "truncated") data = {200, 0, 1, 255};
    else if (mode == "bigram") data = {200, 1, 1, 'a', 128, 1, 128, 0, 0, 1};
    else {
        data = {200, 0};
        for (int i = 0; i < 130; ++i) {
            data.push_back(1); data.push_back('a');
            int next = 2 + (i+1)*5;
            data.push_back(64 | ((next>>16)&63));
            data.push_back((next>>8)&255); data.push_back(next&255);
        }
        data.push_back(0);
    }
    latinime::Dictionary dict(data.data(), 1, 1, data.size());
    int codes[48*16]; for (int &c : codes) c = -1;
    for (int i=0;i<47;i++) codes[i*16] = 'a';
    unsigned short out[48*60] = {}; int freq[60] = {}; unsigned short word[] = {'a'};
    if (mode == "bigram") dict.getBigrams(word, 1, codes, 1, out, freq, 48, 60, 16);
    else if (mode != "header") dict.getSuggestions(codes, mode == "deep" ? 47 : 1, out, freq, 48, 18, 16, -1, nullptr, 0);
}
```
