# FM-ATV ontvanger voor Linux en Windows

Ontvanger in C voor analoge FM-ATV met PAL-kleur, FM-mono, FM-stereo en
NICAM-stereogeluid. **Beeld en geluid zijn beschikbaar op zowel Linux als
Windows.** Voor zwakke signalen zijn vrije synchronisatie en frame stacking
beschikbaar. De ontvanger werkt met SDR-hardware, opgenomen IQ en een ingebouwde demo.

![PAL-kleurontvangst op 2335 MHz met de PE1ITR-testkaart](docs/images/pal-color-reception.png)

*Voorbeeld van PAL-kleurontvangst op 2335 MHz bij 20 MS/s. De screenshot toont
een eerdere versie van de gebruikersinterface.*

De ontvanger is ontwikkeld voor **twee toepassingen**:

- **Complete FM-ATV-ontvangst:** beeld met PAL-kleur en FM-mono, FM-stereo of
  NICAM-geluid. Hiervoor zijn een hogere samplerate en voldoende
  ontvangstbandbreedte nodig; de functiegrenzen staan hieronder.
- **DX-ontvangst van zwakke signalen:** vrije synchronisatie en frame stacking
  helpen om beeld uit zwakke of kort oplevende signalen te halen. Frame stacking
  werkt bij live SDR-ontvangst alleen met een samplerate van maximaal **10 MS/s**.
  Kies voor DX, afhankelijk van SDR, computer en verbinding, een samplerate
  tussen **5 en 10 MS/s**. In dit bereik werkt de ontvanger met zwart-witbeeld
  zonder geluidsdecodering.

De onderste instelgrens verschilt per SDR. Voor live DX met stacking gelden:

| SDR | Instelbaar sampleratebereik voor DX met stacking |
| --- | --- |
| PlutoSDR | 5–10 MS/s; 5 en 6 MS/s zijn in de praktijktest gebruikt met videosync |
| HackRF en B210 | 8–10 MS/s |
| SDRplay RSP1B | 8 tot strikt onder 10 MS/s; nog niet op echte hardware getest |

Kies binnen dit bereik een instelling waarbij de sampleaanvoer continu blijft
en de computer de verwerking bijhoudt. Een instelbare samplerate is geen
garantie dat de gebruikte hardware en verbinding die zonder verliezen halen.
De grens van 10 MS/s voor stacking geldt niet voor IQ-bestanden en de demo.

De PAL-timing is 625 lijnen, 15.625 Hz lijnfrequentie en 50 velden / 25
geïnterlinieerde beelden per seconde. De decoder toont elk veld als een beeld
van 640 × 288 pixels, uitgerekt tot 4:3; hij combineert de twee velden niet tot
576 lijnen. PAL-kleur wordt automatisch gedetecteerd wanneer samplerate,
video-IF en ontvangen kleurburst dat toelaten.

## Wegwijzer en repository-inhoud

Begin met [INSTALL.md](INSTALL.md) voor installatie en compilatie, of met
[Snel starten op Windows](#snel-starten-op-windows) als je al een gebouwd pakket
hebt. De [bediening](#bediening) beschrijft de toetsen. Verderop staan uitgebreide
DSP-uitleg, experimenten en historische metingen voor ontwikkelaars.

Deze repository bevat broncode, bouwscripts en documentatie:

| Pad | Inhoud |
| --- | --- |
| `src/` | Ontvanger, DSP en offline testhulpmiddelen |
| `src/nicam_core/` | Geïntegreerde NICAM-decoder met herkomst en eigen licentie |
| `scripts/` | Bouw-, verpakkings-, status- en analysehulpmiddelen |
| `build/` | Lokaal gegenereerde programma's en testresultaten; uitgesloten van Git |

IQ-opnamen, meetuitvoer, firmware, gebouwde pakketten en losse screenshots horen
niet in Git. Alleen de geselecteerde voorbeeldafbeelding in `docs/images/` wordt
meegeleverd voor deze README. Bewaar eigen metingen onder `build/` of `measurements/` en opnamen onder
`captures/`; deze mappen worden genegeerd. Voorbeelden met `pa0boj3.cf32` of
`airscatter-clean-40s_rx2.iq` verwijzen naar lokale opnamen die niet worden
meegeleverd. Vervang ze door je eigen opname met de juiste samplerate.
De demo heeft deze bestanden niet nodig.

De map `tests/` blijft lokaal en wordt niet op GitHub meegeleverd. Verwijzingen
naar `make test`, `make test-*`, benchmarks en Windows-tests verderop beschrijven
de lokale ontwikkelomgeving; deze vereisen de afzonderlijke testbroncode.
De ontvanger bouwen en de demo starten kan zonder die map.

### Licentiestatus

Er is nog geen projectbrede licentie vastgelegd. De meegeleverde NICAM-code
heeft een [Apache-2.0-licentie](src/nicam_core/LICENSE) en een
[herkomstbeschrijving](src/nicam_core/ORIGIN.txt). Die licentie geldt niet
automatisch voor de overige projectcode. Het Windows-verpakkingsscript
verzamelt daarnaast de licentieteksten van meegeleverde dependencies.

## Ondersteuning en benodigde bandbreedte

| Bron | Linux | Windows-pakket |
| --- | --- | --- |
| PlutoSDR / AD9361 | Via optionele libiio >= 0.23 en < 1.0 | Meegeleverd, via USB of netwerk |
| HackRF | Via SoapySDR en SoapyHackRF | Meegeleverd |
| B210 / beschreven Kintex-7-kloon | Via UHD; passende firmware-/FPGA-images nodig | Niet meegebouwd |
| SDRplay RSP1B | Experimenteel; API en SoapySDRPlay3 apart installeren | Experimenteel; API en passende SoapySDRPlay3-module apart installeren |
| IQ-bestanden en demo | Beschikbaar | Beschikbaar |

De RSP1B-integratie is nog niet met echte RSP1B-hardware getest.
De meegeleverde softwarelibraries vervangen geen eventuele USB-apparaatdriver.

| Functie | Minimale samplerate | Minimale video-IF |
| --- | --- | --- |
| Automatische PAL-kleur | 12 MS/s | 10 MHz |
| FM-mono op 6 MHz | 14 MS/s | 14 MHz |
| NICAM-stereo op 6,552 MHz | 14 MS/s | 14 MHz |
| FM-stereo op 7,02/7,22 of 7,38/7,56 MHz | 16 MS/s | 16 MHz |

Deze grenzen gelden op beide besturingssystemen en maken de decoder beschikbaar;
het ontvangen signaal moet de betreffende beeld- of geluidsinformatie bevatten.
Voor kleur met geluid is 20 MS/s met 16 MHz video-IF een bruikbaar uitgangspunt.
Bij 10 MS/s is er geen geluidsdecodering. Frame stacking bij live SDR-ontvangst
is beschikbaar tot en met 10 MS/s; voor bestanden en demo geldt die beperking niet.

## Snel starten op Windows

1. Pak het volledige Windows x64-pakket uit en installeer zo nodig de
   USB-driver volgens `LEESMIJ.txt` in het pakket.
2. Start `atv-rx.exe`. Kies **W + Enter** om de ontvanger in te stellen,
   vervolgens **2** voor HackRF of **3** voor PlutoSDR.
3. Stel frequentie, eventuele converter-LO, samplerate en video-IF in en kies
   de gewenste geluidsuitgang. Zonder opgeslagen instellingen start **Enter**
   op het eerste scherm de demo.
4. Druk tijdens ontvangst op **W** om de instellingen te bewaren.

**N** wisselt de geluidsbron, **A** dempt of activeert geluid en **! (Shift+1)**
toont de bediening. Laat alle DLL's, de map `modules` en `font.ttf` bij het programma staan.

Meer informatie: [Windows-pakket en bouwen](#windows-x64-testversie),
[Linux bouwen en testen](#bouwen-en-testen), [bediening](#bediening),
[frame stacking](#frame-stacking-bij-korte-signaaloplevingen),
[FM-mono](#mono-fm-geluid-op-6-mhz),
[FM-stereo](#fm-stereo-op-702722-en-738756-mhz),
[NICAM](#nicam-op-6552-mhz-en-geluidsbron-kiezen) en
[ontwerp en grenzen](#ontwerp-en-huidige-grenzen).

## Bouwen en testen

Zie [INSTALL.md](INSTALL.md) voor pakketinstallatie, concrete compileercommando’s,
tests en de Windows x64-cross-build op Fedora.

Voor Linux benodigd: C11-compiler, make, pkg-config en ontwikkelpakketten voor SDL2,
FFTW3 (single precision), libpng, UHD, SoapySDR, FreeType en Fontconfig.
De schermtekst gebruikt een door Fontconfig gekozen monospace-lettertype
met vloeiende randen; installeer bijvoorbeeld Noto Sans Mono of DejaVu Sans Mono. HackRF gebruikt daarnaast de
SoapyHackRF-module. Alle applicatiecode is C; UHD en SoapySDR leveren C-API's
voor hun onderliggende libraries. Pluto gebruikt optioneel **libiio >= 0.23 en < 1.0**
(`libiio-dev`), automatisch gedetecteerd met pkg-config. Zonder libiio blijven
de andere bronnen beschikbaar; `--pluto` meldt dan dat de driver ontbreekt.
Na installatie van libiio: `make -B`. Uitschakelen kan met `make -B HAVE_LIBIIO=0`.

```sh
make
./build/atv-rx --demo
```

`make` bouwt alleen de ontvanger (`build/atv-rx`). De offline hulpmiddelen
`atv-tx` en `atv-eval` bouw je apart met `make tools`.

De demo genereert FM-IQ met PAL-achtige veldsync, egalisatiepulsen,
halvelijnverschuiving, grijsbalken en dambord. Die IQ gaat door dezelfde IF-
en decoderketen als echte ontvangst. Het is een functionele testbron, geen
gecertificeerde PAL-referentiegenerator. Er wordt niets uitgezonden.

## SDRplay RSP1B (experimenteel)

Kies **4. SDRplay RSP1B** in de wizard of gebruik `--sdrplay`.
De aansluiting gebruikt **SoapySDRPlay3** met de officiële **SDRplay API 3.15
of nieuwer**. De API/USB-driver en Soapy-module zijn aparte installaties;
ze worden niet door `make` geïnstalleerd en zitten niet in onze Windows-ZIP.
Er is nog geen ontvangsttest met een echte RSP1B uitgevoerd.

Deze integratie accepteert **8 MS/s tot strikt onder 10 MS/s**, met maximaal
**8 MHz analoge bandbreedte**. De wizard begint bij wisselen naar SDRplay met
8 MS/s en 8 MHz analoge/video-IF. De bestaande decoder vereist minstens
8 MS/s; lagere hardware-samplerates worden daarom niet aangeboden.
Op deze rates werken zwart-witbeeld en stacking; de bestaande PAL-kleur-
en geluidsdecoders vereisen hogere samplerates en blijven uitgeschakeld.
Een brede FM-ATV-uitzending kan door deze beperkte bandbreedte vervormen.

De RSP1B stemt af van **1 kHz tot 2 GHz**. Voor 2,3 GHz ATV is dus een
converter nodig; `--lo` geeft het verschil tussen ontvangst- en SDR-frequentie.
Handmatig afstemmen en AFC blijven binnen de SDRplay-frequentiegrenzen.
De gainbediening regelt **0..39 dB boven de minimale IF-versterking**:
`IFGR = 59 - gain`, met AGC uit en RF/LNA-state 0. Dit is geen gekalibreerde
totale RF-versterking; er is nog geen aparte LNA-statebediening.

### Driver installeren en controleren

Installeer eerst de [officiële SDRplay hardware API](https://sdrplay.com/hardware-api/)
voor het gebruikte besturingssysteem, inclusief de bijbehorende service en
USB-driver. Gebruik daarna een recente
[SoapySDRPlay3-module](https://github.com/pothosware/SoapySDRPlay3)
met RSP1B-ondersteuning, gebouwd voor dezelfde SoapySDR-installatie als FM-ATV.
Gebruik niet de oude SoapySDRPlay-module voor API 2.
Op Linux kan SoapySDRPlay3 na installatie van de API en de SoapySDR-ontwikkelbestanden
bijvoorbeeld uit de bron worden gebouwd:

```sh
git clone https://github.com/pothosware/SoapySDRPlay3.git
cmake -S SoapySDRPlay3 -B SoapySDRPlay3/build -DCMAKE_BUILD_TYPE=Release
cmake --build SoapySDRPlay3/build -j
sudo cmake --install SoapySDRPlay3/build
sudo ldconfig
SoapySDRUtil --info
SoapySDRUtil --find="driver=sdrplay"
SoapySDRUtil --probe="driver=sdrplay"
```

De wizard controleert of de module geladen is; **Installed** bevestigt geen
werkende API-service, aangesloten ontvanger of hardwarecompatibiliteit.
Bij problemen controleer je de SDRplay API-service en of een andere toepassing
de ontvanger al gebruikt. Met meerdere RSP's selecteer je `--args serial=...`;
de applicatie voegt `driver=sdrplay` automatisch toe.

Voor Windows moet de SoapySDRPlay3-module passen bij **x64, SoapySDR ABI 0.8
en de MinGW-build** van ons pakket. Zet die module in `modules`, met de benodigde
runtime-DLL's vindbaar voor Windows. Alleen de officiële API installeren is
niet voldoende. De bestaande cross-build bundelt deze extra module nog niet.

Voorbeeld op 1252 MHz, zonder converter:

```sh
./build/atv-rx --no-config --no-wizard --sdrplay \
  --frequency 1252000000 --rate 8000000 --bandwidth 8000000 \
  --if-bandwidth 8000000 --gain 30
```

Voor 2335 MHz met een converter-LO van 900 MHz vervang je de frequentie door
`--frequency 2335000000 --lo 900000000`; de RSP1B ontvangt dan op 1435 MHz.
CLI en opgeslagen configuraties worden op bovenstaande grenzen gecontroleerd;
de daadwerkelijke samplerate wordt ook na het openen gecontroleerd.

Hardwaregrenzen: [RSP1B-datasheet](https://sdrplay.com/wp-content/uploads/2026/06/RSP1B_Datasheet.pdf).
`make test-sdrplay` test de bron met gesimuleerde Soapy-aanroepen, inclusief
gainrichting, selectie, rate-readback, frequentiegrenzen en opruimen bij fouten.
Dit vervangt geen USB-/ontvangsttest op Linux of Windows.

## Windows x64-testversie

Linux is het ontwikkelplatform van deze ontvanger. In de praktijktest op
Windows was de performance lager: Pluto en HackRF werkten goed, maar op de
geteste laptop moest de HackRF-samplerate van 20 naar **18 MS/s** worden
teruggezet. De haalbare samplerate hangt af van de computer en de gebruikte
SDR; 18 MS/s is de werkende instelling voor deze laptop, geen algemene
Windows-limiet.

Het Windows-pakket bevat PlutoSDR (libiio 0.26, USB en netwerk) en HackRF
(SoapyHackRF). Pak de volledige ZIP uit en dubbelklik op `atv-rx.exe`.
Het Windows-pakket bevat alleen de ontvanger met zijn benodigde onderdelen;
de offline testhulpmiddelen `atv-eval` en `atv-tx` worden niet meegebouwd.
Kies **W + Enter** in de wizard, vervolgens **2** voor HackRF of **3** voor Pluto.
Enter op het eerste scherm start zonder opgeslagen instellingen de demo.
B210/UHD is niet opgenomen in deze Windows-testbuild.
De doelplatforms zijn Windows 10 vanaf 1903 en Windows 11, x64.

Alle applicatie-DLL's, modules en het lettertype worden meegeleverd.
Een eventueel nog ontbrekende Windows USB-apparaatdriver moet eenmalig worden
geïnstalleerd; zie `LEESMIJ.txt` in het pakket voor de officiële instructies.
Instellingen en `atv-rx.log` staan in `%APPDATA%\PE1ITR\ATV Receiver\`.
**W** bewaart instellingen. Fouten tonen een dialoog met de loglocatie.

De Windows-wizard heeft na de SDR-instellingen een stap **Audio output**.
Kies daar het nummer van je luidsprekers, hoofdtelefoon of USB-geluidskaart;
**0** gebruikt de standaarduitgang van Windows. Enter behoudt de huidige keuze.
Bij meer dan zes apparaten blader je met **N/P + Enter**. Bewaar de keuze met
**W** in het ontvangstscherm. De apparaatnaam wordt opgeslagen, zodat een
andere volgorde van geluidsapparaten de keuze niet verandert. Is de gekozen
uitgang niet beschikbaar, dan probeert de ontvanger de standaarduitgang en
vermeldt dit in `atv-rx.log`.
Bij 10 MS/s is audio niet beschikbaar: FM 6 MHz en NICAM vereisen zowel een
samplerate als video-IF-bandbreedte van minimaal 14 MHz; de 7 MHz-stereobronnen
vereisen minimaal 16 MHz. **N** wisselt de audiobron; **A** dempt of activeert geluid.

De cross-build draait op Fedora met MinGW-w64. Benodigd: `mingw64-gcc`,
`mingw64-gcc-c++`, `mingw64-fftw`, `mingw64-freetype`, `mingw64-libpng`,
`mingw64-libxml2`, `mingw64-libusb1`, CMake, make, pkg-config, curl,
Python 3.12+ en `google-noto-sans-mono-vf-fonts`.

```sh
make windows
# Uitvoer: build/windows/atv-receiver-windows-x64.zip
```

Het buildscript downloadt vastgelegde SDR/SDL/FFTW-versies en controleert SHA-256.
FFTW 3.3.10 wordt voor Windows zelf gebouwd met SSE2-versnelling (beschikbaar
op alle x64-processors). De standaard MinGW-FFTW-DLL heeft die versnelling
niet en kan daardoor de realtime videofilters beperken. De DLL uit de eigen
build krijgt voorrang bij linken en verpakken; AVX is niet vereist.
De overige dependencyversies staan lokaal in `build/windows/BUILD-INFO.txt`,
buiten de ZIP. Het pakket wordt bij elke build opnieuw samengesteld uit
de ontvanger en zijn DLL-afhankelijkheden, zonder restanten van oudere builds.
Het pakket bevat de ontvanger, benodigde onderdelen en licentieteksten;
broncode en bouwscripts blijven in de projectrepository. De pakketcontrole
controleert recursief DLL-imports; Windows-tests kunnen onder Wine worden
uitgevoerd vanuit de distributiemap met de EXE's uit `build/windows/tests`.
Wine-tests vervangen geen ontvangsttest op een echte Windows-pc met SDR.

`build/windows/tests/bench-if-filter.exe` meet de daadwerkelijke IF-filtercode
zonder USB of beeldweergave en vermeldt de geladen FFTW-versie. Voer deze uit
met de DLL's uit het pakket. `make build/bench-if-filter` bouwt dezelfde meting
voor Linux. Dit is een filterbenchmark, geen garantie voor ontvangst zonder drops.

De Windows-build gebruikt ook een versnelde FM-faseberekening: de MinGW
`atan2f`-implementatie gebruikt de trage x87 FPATAN-instructie. De vervanger
verkleint eerst het hoekbereik en gebruikt daarna een polynoom; de test met
alle kwadranten en willekeurige floatwaarden begrenst de absolute fout tot
5e-7 radiaal. Nul, oneindig en NaN behouden hun gebruikelijke gedrag.
De Linux-build blijft de systeem-`atan2f` gebruiken. Windows wordt net als
Linux met `-fno-math-errno` gebouwd, zodat onder meer pixelafronding rechtstreeks
CPU-instructies kan gebruiken; `-ffast-math` wordt niet ingeschakeld.
`build/windows/tests/bench-pipeline.exe` meet apart de verwerking met PAL-kleur,
FM-audio, NICAM-detectie en NICAM-decodering. Een meting onder Wine bij 20 MS/s
verbeterde met FM-audio en NICAM-detectie van 2,01 naar 0,98 seconde verwerking
per seconde IQ. Dit sluit USB, beeldweergave en stacking uit: ook onder
1 seconde is nog extra marge nodig voor betrouwbare live ontvangst.

FM-mono, FM-stereo en de NICAM-voorbewerking draaien op een eigen
`Audio-DSP`-thread. De videothread levert daarvoor ruwe FM-samples vóór de
video-de-emphasis aan een wachtrij van ongeveer 100 ms (afgerond op hele
ontvangstblokken). Bij een volle wachtrij vervalt de wachtende audio en reset
alleen de audiodecoder; de videothread wacht niet op audioverwerking.
De NICAM-decoder behoudt daarnaast zijn eigen thread. Bij afstemmen, pauzeren
of wisselen van audiobron wordt oude audio gewist; bij bestandseinde wordt
resterende audio verwerkt.

De video-de-emphasis gebruikt een reële FFT en verwerkt rechtstreeks de
videosamples, zonder omzetting naar complex IQ. De FIR-coëfficiënten en
filtervertraging blijven gelijk. `build/bench-if-filter` vergelijkt ook deze
route met de complexe referentie bij ontvangstblokken van 16.384 samples;
de gemeten winst geldt voor de de-emphasis, niet voor de hele ontvanger.

## Opstartconfiguratie en wizard

Start met `./build/atv-rx`. De wizard verschijnt in het applicatievenster:
**Enter** slaat hem over en gebruikt de opgeslagen instellingen; **w + Enter**
begint met de hardwarekeuze: **1 B210**, **2 HackRF**, **3 PlutoSDR**, **4 SDRplay RSP1B**.
Naast elke SDR staat een indicatie voor continue ontvangst: B210 tot 61,44 MS/s
via USB 3, HackRF tot 20 MS/s, Pluto circa 6–8 MS/s via USB (praktijkindicatie)
en SDRplay 8 tot onder 10 MS/s (huidige applicatiegrens; nog niet op hardware getest).
Dit zijn geen garanties voor de volledige decoder: pc en verbinding bepalen
de haalbare snelheid. De wizard vermeldt ook dat FM 6 MHz/NICAM minimaal
14 MS/s en 14 MHz video-IF vereist. De hints wijzigen geen instellingen of limieten.
De grafische hardwarelijst toont **Installed** in groen of **Not installed**
in rood. Dit controleert de beschikbare driverondersteuning: UHD voor B210,
een succesvol geladen SoapyHackRF-module voor HackRF en libiio-ondersteuning
in deze build voor Pluto en een geladen SoapySDRPlay3-module voor SDRplay. Hiervoor wordt geen SDR gezocht of geopend.
Daarna volgen samplerate, ontvangstblokgrootte, video-IF,
converter-LO, de-emphasis, experimentele sync en de
apparaatspecifieke opties. Enter behoudt de getoonde waarde. Ongeldige invoer
wordt opnieuw gevraagd; frequenties en bandbreedtes worden in Hz ingevoerd.
De sync/alignment-bandbreedte wordt in de wizard automatisch op **3200000 Hz
(3,2 MHz)** gezet, zonder aparte vraag, op zowel Linux als Windows.
De wizard leidt op Linux en Windows de analoge bandbreedte automatisch af:
onder 8 MS/s **samplerate − 1 MHz**, dus 500 kHz ruimte aan beide zijden;
vanaf 8 MS/s **samplerate − 2 MHz**, dus 1 MHz ruimte aan beide zijden.
Bijvoorbeeld 5 MS/s → 4 MHz, 6 MS/s → 5 MHz, 10 MS/s → 8 MHz en
20 MS/s → 18 MHz. De berekende waarde staat bij de
ontvangstblokgrootte; een aparte invoerstap is niet meer nodig. De SDR-driver
kan deze aanvraag afronden naar een ondersteunde hardwarebandbreedte.
Bij overslaan van de wizard blijft de opgeslagen of met `--bandwidth`
opgegeven waarde behouden. De digitale video-IF blijft een aparte instelling.
Bij een andere SDR of gewijzigde samplerate stelt de wizard hiervoor
**samplerate − 2 MHz** voor; Enter accepteert die waarde. Bij dezelfde SDR en
samplerate blijft de bestaande video-IF behouden zolang deze binnen de
samplerate past. Gain bedien je met de pijltjes; de driver begrenst de waarde
bij openen en afstemmen tot het bereik van de SDR.
De ontvanger gebruikt standaard **4000000 Hz (4 MHz, sync-tot-wit)** voor
het videoniveau, onafhankelijk van de SDR-bandbreedte. De wizard vraagt hier
niet meer naar en oude opgeslagen deviatiewaarden worden genegeerd.
Alleen voor tests en IQ-bestanden blijft `--deviation` beschikbaar.
Een smallere SDR-bandbreedte verandert de zenderdeviatie niet.
De hardwaredriver controleert bij openen ook wat het aangesloten apparaat ondersteunt.
De blokgrootte kies je met een lijstnummer uit vaste waarden van 2048 tot
1048576 samples (machten van twee). De lijst toont de blokduur bij de gekozen
samplerate, de huidige waarde en de standaard voor de gekozen SDR. Deze
applicatie gebruikt voor B210 standaard 16384 samples (keuze 4), voor HackRF 32768 samples (keuze 5),
en voor PlutoSDR 131072 samples (keuze 7). Bij wisselen van hardware in de
wizard wordt de bijbehorende standaard gekozen. Bij dezelfde hardware
behoudt Enter de huidige waarde, ook bij een eerder via
`--rx-block` of het configuratiebestand ingestelde afwijkende blokgrootte.

Druk op **! (Shift+1)** voor het infoscherm met het doel van de applicatie,
bouwer **Rob Hardenberg, PE1ITR (rob@pe1itr.com)** en alle toetscommando's.
Ook het versienummer en de bouwdatum/-tijd in UTC staan hier. De timestamp
wordt bij compileren vastgelegd, zodat hij de gebouwde versie identificeert.
`./build/atv-rx --version` toont dezelfde informatie in de terminal.
Het versienummer staat in `src/version.h`.
Elke volgende toets keert terug naar het hoofdscherm zonder het bijbehorende
commando uit te voeren. Ontvangst en verwerking pauzeren zolang het infoscherm zichtbaar is.
Na een toets hervat de applicatie; bij live SDR begint de ontvangst weer
bij het actuele signaal. De ingestelde looptijd telt de infopauze niet mee.

Druk op **T** om een frequentie direct in MHz in te typen, bijvoorbeeld `2335`,
`2335.250` of `10200`. **Enter** stemt af; **Esc** annuleert de invoer.
Een decimale komma werkt ook. Backspace verwijdert het laatste teken;
Ctrl+A maakt het invoerveld leeg. Tijdens invoeren blijven andere sneltoetsen
uitgeschakeld. Je voert de weergegeven frequentie inclusief converter-LO in:
voor bijvoorbeeld 10200 MHz moet de ingestelde LO een SDR-frequentie tussen
0 (exclusief) en 6000 MHz opleveren. Ongeldige invoer blijft in het veld staan
om te corrigeren. Met **W** bewaar je daarna de frequentie als opstartinstelling.

Druk in het hoofdscherm op **W** om de volledige actuele configuratie op te slaan
in **`~/.atv-rx.conf`** op Linux of
**`%APPDATA%\PE1ITR\ATV Receiver\atv-rx.conf`** op Windows. Het scherm bevestigt het opslaan; bij fouten staan de
details in de terminal (Linux) of het logbestand (Windows). Ook frequentie (inclusief converter-LO), gain, video-IF,
inversie, vrije sync, lijnklok, verticale positie, stacking (aantal, rolling en
pauze) en audiokeuze, mute en kanaalwisseling worden bewaard. Een volgende start
laadt deze waarden automatisch. De wizard zelf slaat pas op wanneer je W indrukt.
Snapshot-/opnamebestanden en een tijdelijke looptijd worden niet bewaard.

Commandoregelopties overschrijven geladen instellingen; wijzigingen in de wizard
worden daarna toegepast. Gebruik `--no-wizard` voor scripts en automatisch starten.
De wizard werkt ook zonder terminal, met dezelfde invoercontrole en keuzelijsten.

```sh
./build/atv-rx --no-wizard              # direct starten met opgeslagen instellingen
./build/atv-rx --no-config              # beginnen met standaardwaarden
./build/atv-rx --config mijn-radio.conf # een bestaand configuratiebestand gebruiken
./build/atv-rx --no-config --config mijn-radio.conf # nieuw profiel; opslaan met W
```

`--wizard` toont de grafische wizard. `--terminal-wizard` gebruikt desgewenst
de oorspronkelijke terminalbediening. Het bestand gebruikt leesbare
`naam=waarde`-regels; ongeldige regels geven een fout met het regelnummer.
Opslaan vervangt het bestand atomair. Met `--no-config` kun je een fout bestand
overslaan en met W opnieuw schrijven.

## OpenSourceSDRLab B210 Kintex-7 — TRX1

De aansturing gebruikt de UHD C-API: hostformaat `fc32`, USB-formaat `sc16`, ingestelde klokreferentie en
teruglezen van de werkelijk ingestelde samplerate.

**Standaard: kanaal 0, antenne `TX/RX`, dus de fysieke TRX1-poort.**
Alleen een RX-stream wordt geopend.

```sh
./build/atv-rx --b210 --frequency 1252000000 --rate 20000000 \
  --bandwidth 18000000 --if-bandwidth 18000000 --gain 30
```

De frequentie hierboven is een voorbeeld. Specifiek apparaat of externe referentie:

```sh
./build/atv-rx --b210 --args 'serial=JOUW_SERIENUMMER' --ref external \
  --frequency 1252000000 --antenna TX/RX --channel 0
```

Voor deze kloon zijn passende Kintex-7-images nodig. Standaard zoekt de ontvanger
in `uhd-images/` ten opzichte van de huidige werkmap. Geef een andere map met
`--uhd-images-dir /pad/naar/kintex-images` op. Firmware wordt niet meegeleverd.
De toepassing controleert
of `usrp_b210_fpga.bin` en `usrp_b200_fw.hex` leesbaar zijn en zet
`UHD_IMAGES_DIR` vóór het openen van UHD. Ze valt niet terug op de systeemimages.
De controle verifieert aanwezigheid, niet de inhoud/compatibiliteit van images.
Standaard-Ettus-images zijn voor deze kloon niet de juiste keuze.

## PlutoSDR / AD9361 — RX1

De directe libiio-aansturing gebruikt ontvangstkanalen volgens het [AD9361-voorbeeld van Analog Devices](https://github.com/analogdevicesinc/libiio/blob/v0.26/examples/ad9361-iiostream.c).
Zonder URI wordt één AD9361 met RX-ondersteuning via USB geselecteerd.
Bij meerdere apparaten is `--pluto-uri` verplicht. Gebruik `iio_info -s`
om de actuele USB-URI te vinden; die kan na opnieuw aansluiten veranderen.

```sh
./build/atv-rx --pluto --frequency 1252000000 --rate 10000000 \
  --bandwidth 8000000 --if-bandwidth 6000000 --deviation 4000000 --gain 30

# Expliciete USB-selectie (vervang door de URI uit iio_info -s):
./build/atv-rx --pluto --pluto-uri usb:3.19.5 --frequency 1252000000 \
  --rate 10000000 --bandwidth 8000000 --if-bandwidth 6000000 --deviation 4000000

# Via netwerk, met dezelfde ontvangstinstellingen:
./build/atv-rx --pluto --pluto-uri ip:192.168.2.1 --frequency 1252000000 \
  --rate 10000000 --bandwidth 8000000 --if-bandwidth 6000000 --deviation 4000000
```

De driver gebruikt RX1 (`--channel 0`), RF-poort `A_BALANCED`,
`--ref internal` en handmatige ontvangstgain in dB. `--antenna` is alleen
voor UHD. Frequentie, gain, samplerate en analoge bandbreedte worden
teruggelezen; apparaatfouten worden gemeld. De FPGA-samplerate moet gelijk
zijn aan de PHY-samplerate. De frequentiegrenzen hangen af van de hardware
en firmware; de driver wijzigt de firmware niet.

De bestaande afstemming, converter-LO, IQ-opname en ontvangstqueue werken
ook met Pluto. Ondertekende 12/16-bit IQ wordt naar float32 omgerekend.
De omzetting verwerkt bytevolgorde, bitverschuiving en teken direct, zonder
libiio-aanroep per sample. Bij starten toont het log het I/Q-formaat;
bij afsluiten toont `Pluto RX timing` de gemiddelde libiio-refilltijd en
omzettijd per blok. `read throughput` meet samples per seconde tijdens die
twee stappen, inclusief refill-timeouts, maar exclusief verdere verwerking.
Bij opnieuw afstemmen wordt de IIO-buffer vernieuwd. `--rx-block` bepaalt
ook de IIO-bufferlengte; libiio gebruikt vier kernelbuffers en een timeout
van één seconde. Stoppen kan daardoor op een lopende refill wachten.

**In de praktijktest van de gebruiker werkte Pluto-ontvangst pas met 131072
samples per blok. Daarom is dit nu de Pluto-standaard.** Voor Pluto accepteert
de ontvanger minimaal 5 MS/s; voor de overige bronnen blijft dit 8 MS/s.
Onder 8 MS/s gebruikt de wizard 500 kHz marge aan elke kant: RF-bandbreedte
is samplerate min 1 MHz. Vanaf 8 MS/s blijft de marge 1 MHz per kant.
Bij 5 MS/s stelt de wizard RF- en video-IF-bandbreedte voor op 4 MHz;
bij 6 MS/s op 5 MHz. Het syncpad blijft 3,2 MHz.
5 MS/s vraagt 20 MB/s aan 16-bit I/Q en maakt
een vergelijking met lagere USB-belasting mogelijk, zonder garantie op sync.
Het voorbeeld hierboven vraagt 10 MS/s (40 MB/s aan 16-bit I/Q,
exclusief transportoverhead). De haalbare continue doorvoer hangt af van de verbinding en instellingen;
deze praktijktest garandeert niet alle samplerates en verbindingen.
Grotere blokken verkleinen de overhead per overdracht. De lagere TX-samplerates van DATV
transmitter bewijzen deze RX-doorvoer niet. Het voorbeeld gebruikt een
smalle IF zonder PAL-kleur; kleur vereist hier minimaal 12 MS/s en 10 MHz
video-IF. Frame stacking is bij live Pluto-ontvangst, net als bij de andere
SDR's, beperkt tot 10 MS/s. libiio-refill-overflows worden gemeld als
onderbreking; dit is geen volledige teller van eventuele hardwareverliezen.

`make test` bevat bij een libiio-build ook hardwarevrije Pluto-tests voor
USB-selectie, configuratie, IQ-conversie, retuning, timeouts en foutopruiming.

## HackRF en opgenomen IQ

```sh
./build/atv-rx --hackrf --frequency 1252000000 --rate 20000000 --gain 30
./build/atv-rx --file opname.cf32 --rate 20000000 --deviation 4000000
```

Het bestandsformaat is afwisselend native float32 I,Q, zonder header.
Samplerate en modulatie-instellingen moeten overeenkomen met de opname.
Afstemmen/gain hebben in demo- en bestandsmodus geen effect op de samples;
IF-filter, polariteit en syncinstellingen werken wel.

## Bediening

| Bediening | Functie |
|---|---|
| Links/rechts of muiswiel | Afstemmen in stappen van 100 kHz |
| Shift + afstemmen | Stappen van 1 MHz |
| Ctrl + afstemmen | Stappen van 10 kHz |
| Klik in spectrum | Gekozen frequentie naar het midden afstemmen |
| AFC-knop / `C` | Automatische fijnafstemming aan/uit (live SDR, maximaal ±500 kHz) |
| Omhoog/omlaag | Ontvangstgain ±1 dB |
| `[` / `]` | Digitaal IF-filter smaller/breder, stappen van 1 MHz |
| `F` | Ontvangen sync / vrije sync |
| `,` / `.` | Vrijlopende lijnfrequentie ±0,1 Hz |
| `B` / `V` | Verticale positie in vrije sync een lijn verschuiven |
| `I` | FM-videopolariteit omkeren |
| `D` | FM0 / FM1 wisselen: FM0 voor kleur en detail; FM1 proberen bij zwakke signalen en smalle IF-bandbreedte |
| `A` | Geluid dempen / weer inschakelen |
| `N` | FM 6 MHz → NICAM 6,552 MHz → FM 7,02/7,22 MHz → FM 7,38/7,56 MHz stereo (brede IF) |
| `Shift+N` | Links/rechts omwisselen voor beide FM-stereoparen |
| Stack-knop / `S` | Frame stacking aan/uit |
| FIXED/ROLLING / `M` | Vaste of voortschrijdende stack (begint opnieuw) |
| Knoppen `−` / `+` of toetsen `-` / `+` | Aantal te stapelen veldbeelden (1–512) |
| Reset-knop / `R` | Stapel wissen en opnieuw verzamelen |
| `P` | Volledig venster als PNG opslaan, met ontvangstfrequentie en UTC-tijd |
| Pause/Resume-knop / `Shift+P` | Verzamelen pauzeren/hervatten, resultaat behouden |
| Escape | Stoppen |

De bovenste regels tonen frequentie, gain, samplerate, analoge RF-bandbreedte,
syncstatus, gemiddeld breedband-IQ-niveau in dBFS, aantal gedecodeerde velden
en streamonderbrekingen. Het niveau is niet gekalibreerd in dBm en geen SNR.
Het FFT-spectrum gebruikt een Hann-window en toont ongeveer dBFS per bin
voor een toon, niet dBFS/Hz. De verticale markeringen geven de digitale
IF-bandgrenzen aan. De gele verticale middenlijn markeert de actuele
centerfrequentie. Het spectrum en niveau worden vóór dit filter berekend.

AFC staat bij starten uit. Stem eerst ongeveer op het signaal af en druk `C`
of klik op de AFC-knop rechtsboven. AFC herkent de periodieke PAL-sync
in twee meetvensters en gebruikt het zwartniveau na de kleurburst als
frequentiereferentie (zwart = nul in de FM-demodulator). Zo volgt de meting
niet de gemiddelde beeldhelderheid. De PAL-de-emphasis-versterking wordt in
de frequentiefout verrekend. Correcte videopolariteit en de-emphasis blijven
nodig; bij onvoldoende herkenbare sync wordt niet bijgestuurd.

De correctie blijft binnen **±500 kHz rond de handmatige afstemming**, met
stappen van maximaal 100 kHz en een rustgebied van 10 kHz. Dit is een grens
voor de correctie, geen gegarandeerd vangbereik bij elk signaal of elke IF.
`SEEK` betekent dat een betrouwbare meting ontbreekt, `TRACK` dat AFC het
signaal volgt en `LIMIT` dat de gemeten correctie buiten het bereik valt.
Bij signaalverlies houdt AFC de laatste frequentie vast. Handmatig afstemmen
verplaatst het midden van het vangbereik. Uitschakelen behoudt de laatst
bereikte frequentie. De eerstvolgende stap met pijltjestoetsen of muiswiel
pakt het frequentieraster weer op: vanaf 2335,145 MHz gaat een stap van
100 kHz omhoog naar 2335,200 MHz en omlaag naar 2335,100 MHz. Dit geldt ook
voor het 10 kHz-raster (Ctrl) en 1 MHz-raster (Shift), op de weergegeven
frequentie inclusief converter-LO.

AFC stemt de SDR werkelijk bij: de grote frequentieuitlezing en de
spectrumschaal tonen de teruggelezen frequentie, inclusief converter-LO.
In de knop staat de totale AFC-correctie in kHz. `W` bewaart ook deze afgestemde
frequentie; AFC zelf start steeds uit. Bij demo en IQ-bestanden is AFC niet
beschikbaar. Hardwareafstemming kan kort de beeldsync onderbreken.

## Frame stacking bij korte signaaloplevingen

Bevestigde lijn- én veldsync zijn de primaire uitlijnreferentie. Zolang beide
recent genoeg bevestigd zijn, voegt de ontvanger volledige rasters rechtstreeks
toe: **SYNC ADDING** / **SYNC ALIGN**. Veranderende beeldinhoud kan dan geen
verschuiving veroorzaken of een bijdrage afwijzen. Bij verlies van betrouwbare
sync is beeldvergelijking de terugval (**IMAGE ALIGN**). Bij terugkeer van sync
begint de stapel opnieuw in de ontvangen rastercoördinaten.

Sync en uitlijning gebruiken standaard een apart IF-pad van **3,2 MHz**
(`--alignment-bandwidth 3200000`). Met bijvoorbeeld `--if-bandwidth 8000000`
blijven het livebeeld en het gemiddelde het 8-MHz-signaal gebruiken.
Beeldvergelijking wordt alleen bij ontbrekende sync gebruikt.
Beide filters hebben dezelfde vertraging en beide beelden delen
dezelfde rastercoördinaten. Met `--free-sync` houdt het livebeeld zijn eigen klok.
De UI toont de twee bandbreedtes bij **IF** en **ALIGN**. De toetsen `[` en `]`
wijzigen alleen de video-IF; de afzonderlijke uitlijnbandbreedte blijft vast.
Met `--alignment-bandwidth 0` gebruikt alles de gewone video-IF.
Waar de bandbreedte dit toelaat, wordt het smalle pad na het IF-filter op een lagere samplerate gedemoduleerd
(bij 10 MS/s en 2 MHz IF: 5 MS/s). De rasterklok en het brede videopad blijven
op de oorspronkelijke samplerate. Bij gelijke video- en uitlijnbandbreedte
wordt hetzelfde filterresultaat hergebruikt. Rasterberekeningen worden per
lijn uitgevoerd; het tekenen van de UI houdt de ontvangstmutex niet bezet.
Het extra pad vraagt nog steeds rekentijd; controleer bij live ontvangst ook
**DROPS**. Een smallere bandbreedte kan het signaal
vervormen; de standaard van 3,2 MHz volgt uit de live- en scattertests en is
geen universele optimale waarde. De optie blijft handmatig instelbaar.

Voor onderzoek aan een live signaal kun je `--record build/live-weak.cf32
--record-seconds 3 --seconds 5` aan het ontvangstcommando toevoegen. Dit bewaart
maximaal drie seconden onbewerkte IQ vóór het digitale IF-filter in geheugen
en schrijft die bij afsluiten als native complex float32 (I,Q). Bij 10 MS/s
is dat 240 MB. Een bestaand bestand wordt niet overschreven. Een onderbreking
of wijziging van frequentie/gain tijdens het verzamelen start de opname opnieuw;
bij vroegtijdig afsluiten wordt de beschikbare, eventueel kortere opname bewaard.
De bovengrens is vijftien seconden en 2 GiB (de eerst bereikte limiet geldt).
Voor periodieke audio-uitval kun je bij 18 MS/s tien seconden opnemen met
`--record-seconds 10`; hiervoor wordt circa 1,44 GB RAM gereserveerd.
Speel de opname terug met `--file` en
dezelfde `--rate`, `--deviation` en polariteit; de IF-bandbreedte mag je variëren.
De opname bevat geen metadata, dus bewaar het gebruikte ontvangstcommando.

Klik op **STACK OFF** of druk `S` om stacking aan te zetten. Stel met
de knoppen **− / +** het gewenste aantal in (standaard 25, bereik 1–512).
Het gemiddelde wordt al tijdens het verzamelen getoond. **HOLD** betekent
dat de ontvanger op bruikbare velden wacht; het opgebouwde beeld blijft staan,
ook tijdens langere ruisperiodes met afgekeurde sync of een handmatige pauze. Een volgende
signaalopleving vult dezelfde stapel verder aan.

Boven het linkerbeeld staat de reden van de laatste stackbeslissing:
**INCOMPLETE FRAME** (onvolledig raster), **SEEK IMAGE DETAIL**
(onvoldoende beeldstructuur), **CONFIRM WEAK IMAGE** (wachten op bevestiging),
**NO IMAGE MATCH** (uitlijning afgewezen) of **ADDING** (bijdrage toegevoegd).
Bij wegvallende sync loopt de rasterklok door en mag beeldvergelijking de
stack starten. Recente lijn- of framesync is daarvoor geen voorwaarde;
onvolledige rasters en onvoldoende betrouwbare beelden blijven afgekeurd.
Een zwak kandidaatbeeld blijft beschikbaar voor maximaal acht mislukte
beeldvergelijkingen. Een tussenliggend ruisbeeld komt niet in het gemiddelde;
een later passend beeld kan de kandidaat alsnog bevestigen. Die bevestiging
gebruikt beeldvergelijking en vereist niet opnieuw zelfstandige beeldstructuur.

Met **FIXED / ROLLING** of **M** kies je tussen:

- **FIXED** (standaard): verzamelen tot het doel, daarna **DONE** en bevriezen.
- **ROLLING**: de laatste N geaccepteerde, uitgelijnde velden middelen. Na het
  vullen vervangt elk nieuw geaccepteerd veld het oudste: één erin, één eruit.
  De teller blijft bijvoorbeeld 5/5 terwijl **ADDING** aangeeft dat er wordt
  ververst. Afgekeurde velden, pauze en HOLD schuiven niets door.

**RESET / R** begint opnieuw in de gekozen stand. Wisselen van stand begint
ook opnieuw. In FIXED laat verhogen van het aantal de bestaande stapel verder
verzamelen; verlagen tot onder de huidige teller begint opnieuw. In ROLLING
begint elke wijziging van N een nieuw venster. Er wordt per pixel bijgehouden
welke velden dekking hebben; weggevallen randinformatie blijft niet eindeloos
in het voortschrijdende gemiddelde staan.

Direct starten met vijf voortschrijdende bijdragen:

```sh
./build/atv-rx --file build/drift-noise3.cf32 --free-sync \
  --stack-frames 5 --stack-rolling
```

ROLLING bewaart de uitgelijnde bijdragen: ongeveer 1,8 MiB extra bij N=5,
tot 180 MiB bij N=512. Aan het bestandseinde blijft het laatste venster staan.
Alleen geaccepteerde velden vervangen oudere; bij uitval is het dus geen venster
van een vaste tijdsduur. Grote veranderingen waarvoor uitlijning niet meer
betrouwbaar lukt kunnen nog steeds HOLD geven en een reset nodig maken.

De huidige decoder levert 288-lijns zwart-witvelden. Met ontvangen sync
gebruikt stacking dezelfde interlacefase: maximaal circa 25 bijdragen per
seconde. In freerunning is de echte interlacefase onbekend; daar worden
alle lokale velden op beeldinhoud uitgelijnd (maximaal circa 50/s).
De teller **FRAMES** telt toegevoegde veldbeelden, geen volledige
576-lijnsbeelden. Naast de stack blijft het oorspronkelijke livebeeld zichtbaar.

Stacking zoekt automatisch horizontale en verticale verplaatsing en verandering
van horizontale scheefstand ten opzichte van het eerste bruikbare veld. Je hoeft
de vrije lijn- of veldklok hiervoor niet af te regelen en geen codegebied te
selecteren. De registratie gebruikt beeldinhoud en subpixelinterpolatie.
Ontbrekende randen tellen niet als zwart mee: iedere pixel heeft een eigen teller.
Het referentieraster blijft vast, ook na een pauze of signaalonderbreking.
Een zwak referentiebeeld, en in ROLLING ook een sterk referentiebeeld, wordt
in dat raster bijgewerkt met de huidige stack;
alleen gebieden met voldoende dekking worden bijgewerkt.

Een nieuw referentieveld moet contrast en ruimtelijke samenhang hebben. Voor
registratie worden de beelden ruimtelijk gefilterd, zodat brede letters ook
onder sterke pixelruis gevolgd kunnen worden. Bij zwakke velden wordt samenhang
buiten het filterbereik gemeten: alleen de samenhang die het filter zelf in ruis
maakt is onvoldoende. Een zwakke referentie vereist bevestiging door een volgend
onafhankelijk veld voordat de eerste bijdrage wordt toegevoegd. De minimale
genormaliseerde correlatie is 0,30 bij zwakke velden en 0,80 bij sterke velden.
Bij zwakke velden moet de overeenkomst daarnaast minstens vier standaardafwijkingen
boven het gemiddelde van de onderzochte uitlijningen liggen. Dit is een
heuristiek voor een onderscheidbare piek, geen statistisch gekalibreerde kans.
Na acquisitie levert de overeenkomst met de referentie de vereiste samenhang;
een individueel zwak veld hoeft niet opnieuw de acquisitiedrempel te halen.
Grote scheefstandcorrecties worden bij zwakke velden afgeremd om het volgen
van ruisdetails te beperken.
Na zes geaccepteerde velden wordt de beweging geschat uit maximaal 24 recente
uitlijningen. De ontvanger meet die beweging zelf; de timingoffsets van de zender
hoeven niet bekend te zijn. Bij freerunning houdt de schatting rekening met het
omlopen van de vrije rasterfase, inclusief de blanking buiten het zichtbare beeld.
Een losse, zwakke match die sterk van de voorspelde beweging afwijkt wordt
niet toegevoegd. Een echte fasesprong kan opnieuw worden geaccepteerd na een
bevestigende match binnen vier velden, of direct bij een zeer sterke overeenkomst.
Als ook de bewegingssnelheid verandert, kunnen drie opeenvolgende matches
(met maximaal vier velden ertussen) een nieuwe beweging bevestigen.
Kleine positie- en scheefstandschommelingen worden bij zwakke velden gedempt als
de overeenkomst op beeldinhoud daarmee vrijwel even goed blijft. Er wordt nooit
een veld toegevoegd uitsluitend op basis van de voorspelling.

Na een IQ-onderbreking, bestandslus of handmatige pauze wordt alleen de
bewegingshistorie gewist; de stack blijft behouden. Zo wordt een nieuwe
rasterfase niet ten onrechte als voortzetting van de oude klok behandeld.

De extra verfijning op individuele pixels wordt bij zwakke velden overgeslagen
om uitlijning op toevallige ruisdetails te voorkomen. Onvoldoende
structuur of overeenkomst geeft **HOLD**: het opgebouwde beeld blijft staan.
**AUTO ALIGN** toont de correlatie en de gevonden X/Y-verplaatsing en scheefstand
(in pixels). Dit is een heuristische kwaliteitscontrole, geen garantie dat alle
ruis of dubbelzinnige patronen worden herkend. Een eerste zwak/gestoord
referentiebeeld kan een **RESET / R** nodig maken.

Het zoekbereik is ±320 pixels horizontaal, ±144 lijnen verticaal en maximaal
64 pixels verschil in horizontale verschuiving tussen boven- en onderkant.
Minstens 45% van het verkleinde referentieraster moet overlappen. Beeldinhoud die
uit het raster verdwijnt of omloopt wordt niet gereconstrueerd; bij onvoldoende
overeenkomst wacht stacking. Deze eerste versie corrigeert translatie en
lineaire scheefstand, geen willekeurige lokale vervorming of schaalverandering.
De scheefstand die al in het referentieveld zit blijft behouden.

De ontvanger gebruikt in beide weergavemodi dezelfde adaptieve lijn- en veldsync.
Zonder `--free-sync` volgt ook het livebeeld deze sync. Bij ontbrekende sync
blijft het beeld doorlopen met `SYNC SEEK`; het bevriest niet tijdens het zoeken.
`PAL SYNC` verschijnt alleen bij recent bevestigde lijn- én veldsync.
Met `--free-sync` houdt het livebeeld zijn eigen vrije lijn- en veldklok en mag
het blijven rollen, ook wanneer de stack synchroniseert. Alleen het raster voor
stacking volgt dan de ontvangen sync. Toets **F** wisselt tussen beide
weergavemodi. Vóór stacking worden rastercompleetheid en beeldinhoud gecontroleerd.
Dit werkt zonder extra opties, ook na een
bestandslus. De horizontale stackklok volgt de lijnsync
als minstens 24 pulsen met passende breedte en een consistente fase en periode
over minstens 8 ms gevonden worden. Tijdens fades blijft de laatst gevonden
lijnklok doorlopen. Bij onderbroken pulsen zoekt de ontvanger ook in het over
meerdere lijnen gemiddelde syncsignaal, met onafhankelijke bevestiging in drie
delen van het veld. Zonder betrouwbare sync blijft ook ruis zichtbaar.
Voor verticale synchronisatie zoekt de ontvanger de brede PAL-syncgroep in het
gemiddelde videosignaal. Meerdere herhalingen rond 50 Hz bevestigen de veldstart;
daarna worden ook de veldperiode en kleine frequentieafwijkingen gevolgd.
Tijdens fades lopen beide stackklokken door op de laatst gevonden timing.
Bij eerste veldsync of een bevestigde sprong na een fade wordt de stack opnieuw
opgebouwd in het juiste verticale raster. Hierdoor blijft een eerdere, verkeerd
geplaatste boven- of onderrand niet als referentie behouden. Diepe fades en
onvoldoende betrouwbare sync kunnen nog steeds tot ontbrekende randen leiden.
Bij de eerste betrouwbare lijnsync wordt de stackreferentie opnieuw opgebouwd,
zodat een eerder midden in het beeld geplaatste syncbalk niet wordt vastgehouden.
Die nieuwe referentie vereist een recent bevestigde lijnpositie. Een bestaande
stack kan ook tijdens het vrij doorlopen bijdragen blijven aannemen als de
beeldregistratie betrouwbaar is. De zwartreferentie is de afgestemde
FM-referentie; een lokaal veronderstelde achterstoep wordt niet
gebruikt omdat die bij drift op beeldinhoud kan vallen.

**PAUSE / Shift+P** en **RESUME / Shift+P** blijven beschikbaar. De bestaande som en het beeld
blijven bij pauzeren behouden. Na hervatten wordt automatisch opnieuw naar de
referentie uitgelijnd. Zolang de teller nul is, toont ook het grote beeld het livebeeld met
**LIVE - SEEKING ALIGNMENT**. Vanaf de eerste bijdrage toont het de stack;
bij HOLD blijft die behouden.

Afstemmen, gain- en decoderinstellingen wijzigen wist de stapel, zodat
verschillende ontvangstinstellingen niet worden gemengd. Uit- en weer
inschakelen start eveneens opnieuw. Een IQ-bestandslus behoudt de stapel,
maar identieke herhaalde ruis levert geen extra ruisonderdrukking.

Direct starten met stacking:

```sh
./build/atv-rx --demo --stack-frames 25
```

## Status opvragen via het netwerk (debug / AI-client)

De ontvanger heeft een optionele, alleen-lezen HTTP-interface voor actuele
instellingen en ontvangststatus. Hiermee kan een client **vanuit dit project**
ook een ontvanger op een andere laptop uitlezen. Bij een issue kun je het
IP-adres en de poort doorgeven; de client haalt de instellingen op, zodat je die
niet telkens hoeft over te typen of logbestanden hoeft te kopiëren. De machine
waarop de client of AI-agent draait moet de ontvanger via het netwerk kunnen
bereiken. De interface staat standaard **uit**.

Voeg deze opties toe aan je gebruikelijke startcommando:

```sh
./build/atv-rx --no-wizard --status-server --status-bind 0.0.0.0 --status-port 8765
```

| Optie | Betekenis |
| --- | --- |
| `--status-server` | Statusinterface inschakelen |
| `--no-status-server` | Uitschakelen, ook wanneer de opgeslagen configuratie hem inschakelt |
| `--status-bind IPV4` | Lokaal IPv4-adres om op te luisteren; standaard `127.0.0.1` (alleen deze laptop), `0.0.0.0` voor alle IPv4-netwerkinterfaces, of het eigen LAN-adres |
| `--status-port PORT` | TCP-poort, standaard `8765`, bereik 1–65535 |

Alleen een IP-adres of poort instellen schakelt de interface niet in.
**W** bewaart deze instellingen samen met de ontvangerinstellingen. In het
configuratiebestand heten ze `status_enabled=1`, `status_bind=0.0.0.0` en
`status_port=8765`; `status_enabled=0` schakelt de interface uit. Wijzigingen aan
deze drie instellingen worden bij de volgende start toegepast. Een onjuist
bindadres of een bezette poort geeft een opstartfout wanneer de interface aanstaat.

Op de clientmachine, vanuit de projectdirectory (vervang het voorbeeldadres
door het LAN-adres van de ontvangerlaptop):

```sh
# Eenmalig de actuele status ophalen, bijvoorbeeld bij een issue:
python3 scripts/read-status.py 192.168.1.50 --port 8765 --once

# Elke seconde uitlezen; één JSON-object per regel, Ctrl+C stopt:
python3 scripts/read-status.py 192.168.1.50 --port 8765 --interval 1

# Optioneel een meetreeks verzamelen voor analyse:
python3 scripts/read-status.py 192.168.1.50 --port 8765 > receiver-status.jsonl

# Hetzelfde endpoint rechtstreeks benaderen:
curl http://192.168.1.50:8765/status
```

De Python-client gebruikt alleen de standaardbibliotheek en werkt ook op Windows.
Bij tijdelijk verbindingsverlies meldt hij de fout en probeert hij opnieuw;
met `--once` eindigt hij bij een fout met een niet-nul exitcode.

`GET /status` levert JSON met `schema_version: 1`, programmaversie, bouwdatum,
Unix-tijdstempel en de volgende groepen:

| Groep | Gegevens |
| --- | --- |
| `receiver` | Werkelijke en aangevraagde frequentie, SDR-frequentie, converter-LO, samplerate, RF-/video-IF-/uitlijnbandbreedte, gain, FM0/FM1, frequentiezwaai, polariteit, PAL-de-emphasis, blokgrootte, kanaal en experimentele sync |
| `video` | IQ-niveau vóór en na het video-bandbreedtefilter in dBFS, aantal velden, streamonderbrekingen (`DROPS`), sync-lock, kleur ingeschakeld/gelockt, vrije sync, lijnfrequentie en verticale verschuiving |
| `afc` | Ingeschakeld, toestand (0 zoeken, 1 volgen, 2 buiten vangbereik) en ankerfrequentie aan SDR-zijde |
| `stack` | Ingeschakeld, gepauzeerd, rolling, sync, bijdragen/doel, overgeslagen velden, confidence en uitlijnverschuivingen |
| `audio` | Bron (0 FM 6 MHz, 1 NICAM, 2 FM 7,02/7,22 MHz, 3 FM 7,38/7,56 MHz), mute, lock, NICAM-detectie/lock, L/R-wissel, audio-uitgang geopend, `dsp_queued_samples` en cumulatieve `dsp_queue_overflows` |
| `rx_queue` | Actuele/piek/maximale bufferbezetting in samples, weggegooide samples en hardware-overflows |

Frequenties en bandbreedtes zijn in Hz, samplerate in samples/s, gain in dB.
`metrics_available` geeft aan of de eerste signaal-/buffermeting beschikbaar is;
`metrics_age_ms` geeft de leeftijd van die meting. Die wordt ongeveer 25 keer per
seconde bijgewerkt wanneer IQ binnenkomt. `reception_paused`, `done` en `failed`
helpen stilstaande metingen herkennen. De overige status volgt de actuele
applicatiestatus; bedieningswijzigingen kunnen nog op verwerking door de
ontvangstthread wachten. De tellers gelden voor de lopende ontvangersessie;
bufferstatistieken zijn nul bij bronnen zonder ontvangstqueue. Het signaalniveau
is geen gekalibreerde S-meter of SNR-meting.

**Signaalsterkte binnen de ingestelde bandbreedte:** `video.if_signal_dbfs`
meet `10·log10(mean(I² + Q²))` op de uitgang van het video-IF/bandbreedtefilter,
**vóór de FM-detector**. Dit is dus signaal plus ruis binnen de doorgelaten band,
onafhankelijk van de FM-detectorstand en PAL-de-emphasis. Ook de schermwaarde
`IF … DBFS` gebruikt deze meting. Ongeveer 25 keer per seconde wordt één IQ-blok
gemeten; `video.if_signal_age_ms` geeft de leeftijd daarvan. Tot de eerste meting
is `if_signal_dbfs` gelijk aan `null`. De bestaande `video.signal_dbfs` blijft
het breedband-IQ-niveau **vóór** dit filter, zodat bestaande clients blijven werken.
Bij gelijkblijvend ingangssignaal en bandbreedte hoort 10 dB werkelijke extra
RX-versterking ongeveer 10 dB extra IF-niveau te geven, zolang er geen oversturing
is. Het niveau is niet gekalibreerd in dBm en hangt van de ingestelde gain af.

### Video-SNR na de FM-detector

Naast het IQ-vermogen vóór FM is Video-SNR beschikbaar op het scherm en via de
statusclient. Deze meting gebruikt het videosignaal **na de
FM-detector en de gekozen videofiltering/PAL-de-emphasis**, vóór beeldweergave,
zwartniveau-clamping en stacking. De meting verandert het beeld of geluid niet.

**Video-SNR (`VIDEO SNR~`)** is een ongewogen PAL-schatting. Op normale beeldlijnen
wordt de ruis gemeten tussen 9 en 10 µs na het begin van de horizontale sync: het
vlakke einde van de achterstoep, buiten de kleurburst en vóór actieve video.
De verticale onderdrukking wordt overgeslagen. De samples van ongeveer 0,25 s
worden samengenomen; alleen het gezamenlijke gemiddelde wordt verwijderd, zodat
ook laagfrequente ruis en niveauvariaties meetellen. De syncdiepte wordt bepaald
uit dit zwartniveau en de centrale sync-top (1,5–3,5 µs). Voor PAL schatten we
zwart-tot-wit als `7/3 × syncdiepte`:

```text
Video-SNR ≈ 20·log10((7/3 × syncdiepte) / RMS-ruis)
```

De ontvanger verlangt recente horizontale én verticale sync, minimaal 128
bruikbare lijnen en een positieve, statistisch herkenbare gemiddelde syncdiepte.
De gemiddelde syncdiepte moet meer dan vijfmaal de geschatte standaardfout zijn.
De standaardfout volgt uit de spreiding van de syncdiepte tussen de meetlijnen.
Bij een korte onderbreking van de syncbevestiging worden alleen de getroffen
samples en de onafgemaakte meetlijn overgeslagen. Eerder goedgekeurde lijnen
blijven tijdelijk beschikbaar; na sync-herstel kan een venster met minimaal
128 lijnen opnieuw worden beoordeeld. Tijdens ontbrekende sync blijft de
uitkomst ongeldig. Na 250 ms onafgebroken syncverlies worden de meetlijnen
gewist; een onderbroken meetvenster duurt maximaal 500 ms. Afstemmen en
streamonderbrekingen wissen de meting direct. Zo kan de meter ook bij
wisselende syncbevestiging meten zonder eindeloos opnieuw te beginnen.
Er geldt geen ondergrens van 13,4 dB meer: ook een lage Video-SNR kan geldig zijn.
Zonder betrouwbare meting toont het
scherm `--` en geeft de API `null`. De vrije-weergavestand verandert deze regel
niet: de meetvensters volgen altijd de ontvangen sync.

Dit is een **diagnostische schatting**, geen gekalibreerde standaard-video-SNR.
Een standaardmeting gebruikt een bekende luminantieamplitude en vastgelegde
meetfilters; zie de [Tektronix VM700T-handleiding](https://download.tek.com/manual/070964902.pdf).
Hier wordt de amplitude uit sync geschat en bepaalt de actuele ontvangstketen de
meetbandbreedte. Filterringing, afwijkende sync/videoverhouding, burst die buiten
het nominale venster valt en vervorming kunnen de uitkomst beïnvloeden. Ook bij
smalle IF is de meting alleen bruikbaar zolang sync en een vlakke achterstoep
herkenbaar blijven. Vergelijk SNR-metingen daarom met dezelfde detector-,
bandbreedte- en de-emphasisinstellingen.

De statusclient geeft de groep `video_quality`:

| Veld | Betekenis |
| --- | --- |
| `valid`, `age_ms` | Geldigheid en leeftijd van de kwaliteitssnapshot; gepauzeerde of meer dan 1 s oude metingen zijn ongeldig |
| `video_snr_db` | Geschatte ongewogen PAL-video-SNR in dB, of `null` |
| `noise_rms`, `sync_depth` | RMS-ruis en gemiddelde syncdiepte in dezelfde interne video-eenheden; blijven beschikbaar bij een afgewezen meting als ze gemeten konden worden |
| `sync_depth_error` | Geschatte standaardfout van de gemiddelde syncdiepte over de meetlijnen |
| `lines` | Aantal gebruikte lijnen, ook wanneer de meting wordt afgewezen |
| `reason` | `ok`, `collecting`, `no_sync`, `insufficient_lines`, `noise_below_measurement_floor`, `invalid_sync_depth`, `uncertain_sync_depth`, `paused` of `stale` |
| `method` | `pal_back_porch_sync_scaled_unweighted` |

Het scherm toont de reden wanneer Video-SNR niet beschikbaar is. Ongemeten of
verouderde ruismetingen en syncdieptes zijn `null`. De dB-verhouding is geen dBFS,
dBm of RF-C/N. Het bestaande `video.if_signal_dbfs` blijft het totale IQ-vermogen
vóór de FM-detector. Voor Video-SNR is geen handmatige kalibratie nodig.

De interface geeft status door en kan geen instellingen wijzigen. Hij verstuurt
geen IQ-data, beelden of historische logregels. Er is geen authenticatie of TLS:
gebruik hem op een vertrouwd LAN of via een VPN. Sta zo nodig de gekozen TCP-poort
in de firewall van de ontvangerlaptop toe. Niet-blokkerende sockets, maximaal acht
clients en een timeout van twee seconden begrenzen het werk voor de ontvanger;
een verversingsinterval van één seconde is een geschikt uitgangspunt voor analyse.

### Clippingwaarschuwing

Een rode balk `CLIPPING` verschijnt bij grenswaarden in ongefilterde IQ-samples
en blijft twee seconden na de laatste detectie zichtbaar. De blijvende teller
`CLIPPING: ... SAMPLES` telt sinds de ontvangerstart; gain- of frequentiewijzigingen
wissen hem niet. Een IQ-sample telt één keer, ook als zowel I als Q de grens raakt.
Er wordt geen gain automatisch aangepast.

Bij HackRF zijn de grenzen +127/127 en -128/127, overeenkomstig de CF32-conversie
van SoapyHackRF. Bij andere bronnen controleert de detector het genormaliseerde
digitale bereik (-1 tot +1). Dit is geen volledige ADC-overloadmeting: analoge
compressie en clipping die door hardwarefiltering of schaling verborgen blijft,
kunnen onzichtbaar zijn. Positief gemiddeld IQ-vermogen in dBFS is op zichzelf
geen clippingcriterium. De detector bekijkt alle aan de DSP geleverde samples,
geen door de ontvangstbuffer weggegooide data.

De JSON-status `clipping` bevat `active`, de 64-bits tellers `clipped_samples` en
`inspected_samples`, en `last_clip_age_ms` (0 zolang er geen detectie is geweest).
Publicatie volgt de bestaande statusverversing, circa 25 keer per seconde.

### NICAM- en afspeeldiagnostiek

De `audio`-status bevat cumulatieve tellers sinds de ontvangerstart:

- `nicam_good_frames` en `nicam_bad_frames`: beoordeelde frames die slagen
  of worden afgekeurd; losse slechte frames kunnen voorkomen terwijl lock blijft staan.
- `nicam_muted_frames`: PCM-pakketten die wegens ontbrekende lock door stilte
  zijn vervangen. Stilte voor afgekeurde frames binnen de decoder valt hier niet onder.
- `nicam_sync_bits_discarded`: bits overgeslagen bij zoeken/herstellen van
  framesynchronisatie, ook tijdens de eerste acquisitie.
- `nicam_queue_overflows`: overloop van de aparte NICAM-IQ-wachtrij.
- `playback_refills`: lege SDL-afspeelbuffer na het starten; dit geeft risico
  op een hoorbaar gat aan, maar is geen meting van de fysieke audio-uitgang.
- `playback_resets`: afspeelbuffer geleegd wegens te grote achterstand.
- `playback_max_supply_gap_ms`: grootste tijd tussen PCM-aanleveringen,
  exclusief bewuste resets zoals afstemmen en bronwisselen.

Vergelijk toenames tijdens het hoorbare probleem. Een historische piek of
acquisitiefout alleen bewijst geen actuele storing. De NICAM-tellers worden
vanuit de audiowerker gepubliceerd; tijdens inactieve verwerking blijft de
laatste stand zichtbaar.

## FM0 / FM1 vergelijken

Met **D** wissel je tijdens ontvangst tussen **FM0** (de standaard
fasediscriminator) en **FM1** (de 8-punts FFT-fasedetector volgens Linrad).
Eén toets schakelt zowel het video- als het sync-/uitlijnpad. De actieve stand
staat bij de IF-instellingen als `FM0 [D]` of `FM1 [D]`. Start direct met
`--fm-mode 1`; `W` bewaart de huidige keuze in de configuratie (`fm_mode`).
Omschakelen reset de detectoren, sync en stapel zodat beide methoden niet in
één gemiddelde worden gemengd. De laatste afbeelding blijft staan tot nieuw
beeld beschikbaar is.

FM1 kiest de sterkste FFT-bin en bepaalt de fase met die bin en zijn twee
buren. Beide paden gebruiken hiervoor de volledige samplerate en dezelfde
8-samplehistorie, ook wanneer FM0 het smalle pad gedecimeerd verwerkt.
De SSE2-route verwerkt vier opeenvolgende posities tegelijk en leest hun
vensters rechtstreeks uit het IQ-blok. Alleen aan de blokgrens wordt de
historie aangevuld; er worden geen samples overgeslagen. De FFT-berekening,
binselectie en vertraging blijven gelijk.
FM1 kan ruis verminderen, maar ook detail, kleur en audio beïnvloeden:
de audio wordt immers uit de primaire video-FM-uitvoer gehaald. FM1 vraagt
meer CPU; let bij live ontvangst op `DROPS`.

Praktische keuze, op basis van live ontvangst onder Linux bij **20 MS/s**:

- **FM0 (D0)** gaf duidelijk betere kleuren dan FM1. Gebruik deze stand als
  uitgangspunt voor kleur en detail, vooral bij voldoende sterk signaal en
  ruime IF-bandbreedte.
- **FM1 (D1)** gaf vooral enig voordeel bij zwakke signalen en een smalle
  IF-bandbreedte. De FFT-fasedetector kan daar een rustiger beeld opleveren,
  terwijl verlies aan kleur en detail minder zwaar weegt.

Dit is een praktijkrichtlijn, geen gegarandeerde verbetering: vergelijk beide
standen bij dezelfde afstemming en IF-bandbreedte. De beste keuze hangt af van
signaalsterkte, frequentiezwaai en bandbreedte. Een smallere IF laat minder ruis
door, maar kan ook bruikbare beeldinformatie wegnemen.

Reproduceerbare vergelijking met gegenereerde IQ-bestanden:

```sh
make tools test-fm
python3 scripts/compare-fm.py --output-dir build/fm-comparison
python3 scripts/compare-fm.py --output-dir build/fm-pa0boj3 \
  --capture build/pa0boj3.cf32 --rate 18000000 --if-bandwidth 16000000
```

Het script bewaart meetwaarden in `results.json` en bij de synthetische test
ook IQ en PGM-beelden. De evaluator ondersteunt `--fm-mode 0|1` en
`--alignment-bandwidth HZ`. Dit is een monochrome DSP-vergelijking zonder
PAL-kleurdecoder, de-emphasis, audio, GUI of stacking. CPU-tijd is de som
voor seriële DSP-verwerking, geen meting van live drops met twee threads.

Meting op 19 oktober 2026: 20 MS/s, video-IF 8 MHz, uitlijn-IF 3,2 MHz,
0,165 s testkaart, AWGN-seed 42. PSNR van het laatste veld tegen hetzelfde
schone FM0-referentieveld, met vaste vrije rasterklok (hoger is beter):

| C/N over 20 MHz | FM0 PSNR | FM1 PSNR |
|---|---:|---:|
| 12 dB | 25,47 dB | 25,84 dB |
| 6 dB | 20,07 dB | 22,20 dB |
| 0 dB | 12,95 dB | 15,11 dB |
| −6 dB | 7,73 dB | 8,53 dB |

Bij −6 dB herstelde geen van beide detectors de veldsync; bij de andere
niveaus vonden beide 8 velden. Zonder ruis is FM0 per definitie gelijk aan
zijn referentie; FM1 haalt 29,39 dB door verschillen in detail en vertraging.
Deze vergelijking bevat die verschillen en bewijst geen universele FM1-winst.
Bij `pa0boj3.cf32` vonden beide 299/299 velden. De seriële DSP kostte circa
5,8 s CPU met FM0 en 18,7 s met FM1 voor 6 s opname. Voor die opname is er
geen foutloos referentiebeeld en dus geen objectieve beeld-SNR-meting.

### Scatter: volledige ontvanger en rolling stacking

Voor `airscatter-clean-40s_rx2.iq` is alleen `atv-eval` met gewone sync
**geen representatieve ontvangervergelijking**: de echte ontvanger laat zijn
adaptieve rasterklok doorlopen zonder lock en gebruikt beeldvergelijking
voor stacking. Gebruik daarom `bench-scatter`, dat rechtstreeks
`init_decoders`, de ontvanger-framecallback, de IF/FM-paden en de echte
stack-worker aanroept. Het verwerkt alle velden zonder drops; GUI, audio en
real-time scheduling worden niet gemeten.

Volledige vergelijking van 19 oktober 2026: 40,01 s, 20 MS/s, 4 MHz deviatie,
video-IF 6 of 8 MHz, uitlijn-IF 3,2 MHz, **rolling stack 25**, de-emphasis uit
(passend bij de vlak gemoduleerde bron), nominale lijnklok 15625 Hz en
experimentele sync uit. Alle vier runs leverden 2000 doorlopende rasters.

De analyse vergelijkt ieder liveveld en iedere weergegeven stapel met de
bekende uitgezonden `ATV 123`-kaart. Na 4×4 middelen worden rijgemiddelden
verwijderd (tegen dominantie van horizontale strepen); de maximale
normaliseerde kruiscorrelatie over cyclische verschuivingen meet de
beeldstructuur. Dit is **geen SNR in dB of universele leesbaarheidsgrens**.
Stapelbeelden die tijdens een fade blijven staan tellen in de tijdgrafiek
als weergegeven beeld, niet als nieuw ontvangen informatie.

| Video-IF | Beste livecorrelatie FM0 / FM1 | Beste stapelcorrelatie FM0 / FM1 |
|---|---:|---:|
| 8 MHz | 0,300 / 0,324 | 0,801 / 0,828 |
| 6 MHz | 0,368 / 0,377 | 0,844 / 0,855 |

FM0 accepteerde over alle stapelresets samen 273 bijdragen, FM1 316;
de acceptatie wordt door het gezamenlijke uitlijnpad bepaald. Rond
**20,992 s** heeft FM0 vijf bijdragen in de nieuwe stapel, FM1 al 24.
De stapelcorrelatie is daar 0,390 tegenover 0,636 bij 8 MHz en
0,478 tegenover 0,694 bij 6 MHz. Bij deze opleving passeert FM1 de
vergelijkingsdrempel r > 0,5 ongeveer 0,30 s (8 MHz) of 0,28 s (6 MHz)
eerder. De gelijktijdige beelden bevestigen de extra herkenbare structuur.
De piekkwaliteit verbetert minder sterk dan het herstel na deze fade.
Op dit bestand helpt FM1 dus wel; het verschil is kleiner bij 6 MHz.

Reproduceren (circa 3 GB aan verliesloze live-/stapelbeelden):

```sh
make build/bench-scatter
mkdir -p build/scatter-full
./build/bench-scatter build/airscatter-clean-40s_rx2.iq build/scatter-full/fm0-8 0 8000000 1
./build/bench-scatter build/airscatter-clean-40s_rx2.iq build/scatter-full/fm1-8 1 8000000 1
./build/bench-scatter build/airscatter-clean-40s_rx2.iq build/scatter-full/fm0-6 0 6000000 1
./build/bench-scatter build/airscatter-clean-40s_rx2.iq build/scatter-full/fm1-6 1 6000000 1
MPLCONFIGDIR=/tmp/atv-matplotlib OPENBLAS_NUM_THREADS=1 python3 scripts/analyze-scatter.py build/scatter-full
```

Een aparte meting van de **parallelle kritieke ontvangstketen**, zonder andere
benchmarks, GUI, stacking, schijfuitvoer of SDR-I/O, gaf op deze machine
**vóór de FM1-optimalisatie**:

| Video-IF, 20 MS/s | FM0 tijd per seconde IQ | FM1 tijd per seconde IQ |
|---|---:|---:|
| 6 MHz | 0,491 s | 1,719 s |
| 8 MHz | 0,483 s | 1,748 s |

De oorspronkelijke FM1 haalde hier circa 11,4–11,6 MS/s en kon live
20 MS/s niet bijhouden. De kleinere IF verandert de samplerate niet.
De offline beeldwinst is daarom geen aanbeveling voor live FM1 op 20 MS/s;
optimalisatie of een apart geteste lagere samplerate is daarvoor nodig.
Reproduceer de meting zonder andere zware tests tegelijk:

```sh
make build/bench-fm-pipeline
./build/bench-fm-pipeline build/airscatter-clean-40s_rx2.iq
```

De afgesproken grens voor **live stacking is 10 MS/s** (en wordt door de
ontvanger afgedwongen); bovenstaande 20-MS/s-bestandstest mag daarom niet
als oordeel over die liveconfiguratie worden gebruikt. Een aanvullende
meting op 10 MS/s, met hetzelfde scatterfragment na antialias-resampling
20 → 10 MS/s, gaf vóór optimalisatie:

| Video-IF, 10 MS/s | FM0 tijd per seconde IQ | FM1 tijd per seconde IQ |
|---|---:|---:|
| 6 MHz | 0,272 s | 0,918 s |
| 8 MHz | 0,278 s | 0,948 s |

Inclusief de echte rolling-stack-worker (25 velden, 8 MHz video-IF),
pipelining en bestand lezen, maar zonder GUI of beelden wegschrijven,
kostte het 2-secondenfragment 1,070 s met FM0 en 2,070 s met FM1.
Dit was een korte throughputproef, geen langdurige live drop-test: de
oorspronkelijke FM1 zat op deze machine rond de realtimegrens.
De beeldkwaliteitscijfers hierboven blijven specifiek voor 20 MS/s;
de 8-samplehistorie van FM1 beslaat op 10 MS/s tweemaal zoveel tijd.
De throughputbenchmark accepteert optioneel samplerate en starttijd:
`bench-fm-pipeline IQ 10000000 0`. De stackbenchmark accepteert na
`rolling` optioneel `samplerate` en `save-images` (0 voor throughput).

### FM1-optimalisatie

De 8-punts FFT is nu rechtstreeks uitgeschreven. Daarmee vervallen de
algemene FFT-lussen, bitreversal-indexering en vermenigvuldigingen met 0/1.
De sterkste bin wordt met paarsgewijze vergelijkingen gekozen, met dezelfde
keuze bij gelijk vermogen. Voor de volledige samplerate worden opstart en
statusupdates buiten de herhaalde sampleberekening afgehandeld.
Op processors waarvoor SSE2 beschikbaar is, worden vier onafhankelijke
FM1-tijdstippen tegelijk berekend. De faseverschilberekening en het filter
blijven in de oorspronkelijke samplevolgorde. Andere platforms gebruiken
de geoptimaliseerde gewone C-versie; `FM1_NO_SIMD` forceert die terugval.
Het venster, de buur-bincombinatie, `atan2f`, filtering en timing blijven gelijk.

Gemeten op de Ryzen 7 5800H, 10 MS/s met 3,2 MHz uitlijn-IF, zonder andere
zware tests tegelijk:

| Video-IF | FM1 vóór: seconden per seconde IQ | Gewone C na | SSE2 na |
|---|---:|---:|---:|
| 6 MHz | 0,909 | 0,648 | 0,460 |
| 8 MHz | 0,949 | 0,665 | 0,462 |

Met SSE2 is de verwerkingstijd van de parallelle kritieke ontvangstketen
ongeveer gehalveerd. Inclusief rolling stacking van 25 velden en het bewaren
van alle live-/stapelbeelden kostte hetzelfde 2-secondenfragment op 10 MS/s
en 8 MHz video-IF 2,157 s vóór en 1,290 s na optimalisatie.

De volledige `airscatter-clean-40s_rx2.iq` is met antialias-filtering naar
10 MS/s omgezet (SciPy `resample_poly`, factor 1/2, Kaiser 8,6). Alle
**2000 livevelden, stapelbeelden en stapel-/syncbeslissingen zijn byte voor
byte gelijk** aan de oorspronkelijke FM1. Deze volledige proef, inclusief
bestand lezen, deterministische stacking en 737 MB beeldtraces wegschrijven,
kostte 57,007 s vóór en 39,721 s na optimalisatie voor 40,01 s IQ.
Deze tijden bevatten geen GUI of live SDR-I/O en bewijzen geen dropvrije
hardwareontvangst, maar geven duidelijk meer verwerkingsruimte bij 10 MS/s.
Resultaten staan in `build/fm1-optimization/results.json`.

`test-fm` vergelijkt de geoptimaliseerde uitvoer en status met de oorspronkelijke
FFT-implementatie, naast de onafhankelijke double-precision DFT-controle.
Daarbij worden kleine/lege blokken, opstart, polariteit, verschillende
amplitudes, sample-strides en ongefilterde/monochrome uitvoer gecontroleerd.
`make test-fm` test zowel de SSE2- als de gewone C-versie.

Gebruik een nieuwe uitvoermap bij herhaling: de benchmark overschrijft geen
bestaande traces. De analyse vereist NumPy, Pillow en Matplotlib en schrijft
`summary.json`, `quality-over-time.png`, `burst-21s.png` en tijdlijnbeelden.
De `.gray`-bestanden bevatten per CSV-regel eerst live, dan stapel,
beide 640×288 bytes luminantie. Tijden zijn relatief aan het IQ-bestandsbegin.

## Zwakke contestbeelden

`--bandwidth` stelt het analoge filter van de SDR in. `--if-bandwidth` is een
**apart digitaal complex IF-filter vóór de FM-demodulator**, tijdens ontvangst
regelbaar van 250 kHz tot de samplerate. De breedte is de volledige doorlaat,
dus bijvoorbeeld 4 MHz betekent ruwweg −2 tot +2 MHz rond het midden.
De FIR heeft 257 taps, een Blackman-window en wordt via FFT overlap-save
uitgevoerd. De overgangsbreedte schaalt met de samplerate; de opgegeven grens
ligt ongeveer op het −6 dB-punt en is geen ideale scherpe begrenzing.

Begin breed, stem zorgvuldig af, en maak het IF-filter geleidelijk smaller.
Grote letters kunnen herkenbaar blijven terwijl fijne details verdwijnen.
Een smaller filter kan ook gewenste FM-zijbanden afsnijden en de modulatie
vervormen: een verbetering bij zwakke signalen is niet gegarandeerd.
Voorlopig staat het IF-filter midden op de ingestelde ontvangstfrequentie.

Met `F` blijft de decoder een raster tekenen, ook zonder gedetecteerde sync.
De vrije tijdbasis gebruikt standaard 15.625 Hz en 50 Hz; wijzig de lijnklok
met `,` en `.` en de verticale positie met `B` en `V`. Een lopend beeld is
hier toegestaan. Start bijvoorbeeld direct met:

```sh
./build/atv-rx --b210 --frequency 1252000000 --if-bandwidth 6000000 \
  --free-sync --line-hz 15625
```

Met `--experimental-sync` schakel je de experimentele lijn- en veldsync
**samen** in. De optie staat standaard **uit**. Voeg haar toe aan je bestaande
live-ontvangst- of afspeelcommando. Zonder deze optie blijft het oorspronkelijke
syncgedrag behouden. De oude namen `--experimental-vsync` en
`--experimental-echo-sync` blijven als alias werken en schakelen nu ook beide
verbeteringen samen in.

De lijnsync geeft na verwerving voorrang aan een nog voldoende sterke
pulsreeks binnen 2 µs van de bestaande timing. Na drie velden zonder bevestiging
wordt normale herverwerving weer toegestaan. De veldsync kiest over een heel
veld de sterkste syncgroep en behoudt de bevestiging over drie opeenvolgende
velden. Zo kan een vroeg donker beeldpatroon de echte veldsync niet meer door
de bestaande 15 ms wachttijd blokkeren.

Bij `build/pa0boj3.cf32` (18 MS/s) wordt de testkaart weer volledig weergegeven;
bij live ontvangst gaf de combinatie ook een stabieler beeld:

```sh
./build/atv-rx --file build/pa0boj3.cf32 --rate 18000000 \
  --if-bandwidth 16000000 --experimental-sync --loop
```

Het experiment verwijdert geen dubbelbeeld en verandert het audiopad niet.
De veldselectie voegt maximaal ongeveer één veld wachttijd toe, zonder de
videostream te bufferen. Een sterker storingspatroon kan nog steeds verkeerd
gekozen worden; het vasthouden van lijnsync kan herpakken na een echte
timingsprong kort vertragen.

De automatische sync gebruikt een apart laagdoorlaatpad van 500 kHz,
hysterese, pulslengtecontrole en een venster rond de nominale lijnafstand.
Het is nog geen volwaardige PLL of correlatiedetector voor zeer lage SNR.
De syncdrempel is relatief aan de afgestemde draaggolf: grote frequentie-offsets
kunnen lock verhinderen. Zwartniveau wordt op de achterstoep geschat; in vrije
sync kan die meting op een verkeerde plaats vallen en daardoor helderheid
beïnvloeden. Er is nog geen handmatige zwartniveau-/contrastregeling.

`--deviation` bepaalt de schaal in Hz per video-eenheid: sync −0,3,
zwart 0, wit +0,7. Bij 4 MHz is sync-tot-wit dus 4 MHz frequentieverschil.
Dit is niet automatisch dezelfde definitie als de piekdeviatie van een zender.
Kies bij omgekeerde modulatie `--invert` of druk `I`.

## Losse FM-ATV-testzender en ruismetingen

`atv-tx` is een **zelfstandige offline zender in C**: hij maakt een
monochroom PAL-testbeeld met grote **ATV 123**-letters en grijsbalken,
FM-moduleert dit en schrijft IQ naar een bestand. Hij gebruikt een eigen
signaalgenerator, niet de demo-functie of decoder van de ontvanger, en heeft
geen toegang tot radiohardware. De nominale timing bevat 625 lijnen per 40 ms,
50 velden/s, halvelijnverschuiving en egalisatie-/brede syncpulsen. Het is
geen gecertificeerde PAL-conformiteitstestbron.

**Stand van deze eerste versie:** de testbron gebruikt standaard 4 MHz
sync-tot-wit-frequentiespan. Dat is niet hetzelfde als 18 MHz bezette
RF-bandbreedte. De bron is nog niet ingesteld en spectraal gevalideerd als
representatief voor de beoogde uitzending in een 18 MHz FM-ATV-kanaal.
De huidige ruis- en IF-vergelijkingen gelden voor deze synthetische bron;
ze bepalen nog geen ontvangstgrens voor de bedoelde uitzending.
Het doel blijft grote contestletters leesbaar maken bij zwakke signalen,
ook wanneer het beeld vervormt of loopt. Fijne details, correcte grijstinten
en stabiele sync zijn daarbij ondergeschikt.

```sh
make tools
./build/atv-tx --output build/test-clean.cf32 --seconds 1
./build/atv-tx --output build/test-noisy.cf32 --seconds 1 --cnr-db 4 --seed 42
./build/atv-rx --file build/test-noisy.cf32 --loop --if-bandwidth 8000000
```

Aan het bestandseinde blijft het venster met het laatste resultaat open
(**FILE END**) tot je afsluit met Esc; `--seconds` kan de looptijd begrenzen.
`--loop` herhaalt een opname tot je afsluit. Elke herhaling reset IF en sync;
die overgang is geen gesimuleerde RF-uitval en verhoogt de overflowteller
niet. Met `F`, `[` en `]` kun je dezelfde ruisopname vergelijken in vrije
sync en bij verschillende IF-bandbreedtes. Korter dan één volledig veld
levert uiteraard geen bruikbaar herhaald beeld op.

**C/N-definitie:** `--cnr-db` is het vermogen van de constante FM-draaggolf
gedeeld door het totale complexe ruisvermogen **over de volledige
samplerate-bandbreedte**, vóór het IF-filter. Bij 20 MS/s wordt ruis over
20 MHz gemeten. Een lagere waarde geeft meer ruis; zonder `--cnr-db` wordt
er geen ruis toegevoegd. Dit is dus geen audio/video-SNR en geen C/N na
het IF-filter.

Voor amplitude A gebruikt de generator onafhankelijk Gaussisch verdeelde
I- en Q-ruis met elk variantie `A² / (2 × 10^(CNR/10))`. Dezelfde seed en
instellingen geven op dezelfde build reproduceerbare samples, ongeacht
blokgrootte. Bij een C/N-reeks met dezelfde seed wordt dezelfde ruisrealisatie
op verschillende sterktes gebruikt. Verschillende seeds zijn nodig voor
onafhankelijke herhalingen.

De samples zijn float32 I,Q in native bytevolgorde, zonder header, rechtstreeks
leesbaar met `--file`. Er wordt niet geclipt op ±1; ruispieken mogen daarboven
komen. Dit model simuleert AWGN, **geen ADC-clipping, fading, multipath,
frequentiedrift of echte SDR-ruisgetallen**. Alleen de amplitude verminderen
zonder ruis toe te voegen verlaagt de C/N van deze ideale decoder niet.

Naast `test-noisy.cf32` schrijft de zender `test-noisy.cf32.json` met onder
andere samplerate, sample-aantal, deviatie, seed, bytevolgorde en gemeten
ruisvermogen/C/N. De ontvanger leest deze metadata nog niet automatisch:
geef afwijkende `--rate`, `--deviation` en `--invert` ook aan de ontvanger door.
Bestaande generatorbestanden worden niet overschreven.

## Usecase 1: afwijkende lijn- en veldfrequenties

De testzender heeft twee onafhankelijke timingoffsets:

- `--line-offset-hz`: offset ten opzichte van 15625 Hz, bereik −100 tot +100 Hz.
- `--field-offset-hz`: offset ten opzichte van 50 velden/s, bereik −1 tot +1 Hz.

Een veld is een halfbeeld; 50 velden/s komt normaal overeen met 25 volledige
frames/s. Onafhankelijke offsets doorbreken bewust de vaste 625-lijnsverhouding:
dit is een afwijkende testbron, geen standaardconform PAL-signaal. Lijntiming
(inclusief pulsbreedtes) schaalt met de ingestelde lijnfrequentie. Het patroon
heeft 288 beeldrijen; de verticale blanking verandert en bij extreme combinaties
met kortere velden kunnen de onderste patroonrijen worden afgekapt.
De werkelijke frequenties en offsets staan in de JSON naast het IQ-bestand.

Bijvoorbeeld, een ruisvrije opname met beide afwijkingen:

```sh
./build/atv-tx --output build/drift.cf32 --seconds 2 \
  --line-offset-hz 2 --field-offset-hz 0.02
./build/atv-rx --file build/drift.cf32 --free-sync --stack-frames 25
```

Geef de offsets **niet** door aan de ontvanger. Laat voor afzonderlijke proeven
één offset weg; gebruik negatieve offsets voor de andere driftrichting.
`make test` controleert afzonderlijke en gecombineerde drift door de volledige
zender → IQ → IF → freerunning-decoder → stack-keten, plus scheefstand,
randdekking, ruisweigering en opnieuw aansluiten bij de referentie. Er zijn ook
IQ-proeven met C/N −7, −8 en −10 dB, beide timingoffsets en onafhankelijke ruisseeds,
met controle op samenhang met het schone beeld en verbetering ten opzichte van
ongecorrigeerd middelen van dezelfde velden. Pure-ruisproeven moeten op nul
bijdragen blijven staan; ruis na acquisitie mag de opgebouwde stack niet wijzigen.

Andere opties: `--amplitude`, `--offset` (draaggolfoffset in Hz) en `--invert`.
`--deviation` betekent hier, net als bij de ontvanger, sync-tot-wit
frequentiespan. `--help` toont alle opties.

**Bestandsgrootte:** 20 MS/s × 8 bytes per IQ-paar = **160 MB per seconde**.
Een korte opname met `--loop` is daarom handig.

### Automatische meetreeks

```sh
python3 scripts/noise_sweep.py --output build/mijn-ruistest --seconds 0.4 \
  --seed 42 --cnr 20 12 8 4 0 -4 -8 -12 --if-mhz 18 8 4
```

Het uitvoermapje moet nieuw zijn. De reeks bewaart de schone en ruisige
IQ-bestanden, metadata, gedecodeerde PGM-beelden, commandolog en
`results.csv`/`results.json`. De standaardreeks gebruikt ongeveer 576 MB aan
IQ bij 0,4 seconde per bestand. Elke opname wordt met alle gekozen
IF-bandbreedtes en zowel automatische als vrije sync verwerkt.

`atv-eval` gebruikt dezelfde IF- en decoderimplementatie als de GUI, zonder
SDL of SDR. Voor afzonderlijke metingen:

```sh
./build/atv-eval --file build/test-clean.cf32 --output build/referentie.pgm
./build/atv-eval --file build/test-noisy.cf32 --if-bandwidth 8000000 \
  --reference build/referentie.pgm --output build/ontvangen.pgm
```

Het resultaat op stdout is JSON met:

- **fields / expected_fields:** aantal geproduceerde veldbeelden versus het
  theoretische aantal compleet af te sluiten velden in de opname. In vrije
  sync zegt deze verhouding niets over ontvangen sync of leesbaarheid.
- **MAE/MSE/PSNR:** pixelverschil met één schoon referentieveld, gemiddeld over
  alle geproduceerde velden. Meer PSNR is beter. Niet geproduceerde velden
  tellen niet mee in deze beeldmaat; bekijk dus altijd ook de veldverhouding.
- Zonder gedecodeerde velden zijn beeldmaten `null` en wordt geen PGM
  geschreven. Bij een exact identieke vergelijking is PSNR mathematisch
  oneindig: JSON geeft dan `null` met `perfect_match: true`.

De reeks gebruikt per syncmodus een eigen schone referentie, bij de breedste
gekozen IF. Ook de vervorming door een smallere IF telt dus mee. De vergelijking
corrigeert geen verschuivingen: veldfase, acquisitie en interlace kunnen ook
bij een schoon signaal een eindige PSNR geven. Dit is **geen automatische
letterherkenning of universele decodeergrens**. De vrije tijdbasis start in
deze synthetische test met bekende timing; een echte zender kan afwijken.
Beoordeel daarom ook de beelden, test langere opnamen en herhaal met
verschillende seeds.

Optionele grafieken en een overzicht van de beelden (vereist matplotlib):

```sh
python3 scripts/plot_noise_sweep.py build/mijn-ruistest
```

De eerste lokale reeks staat in `build/awgn-sweep` (0,24 seconde per
ruisniveau, seed 1). Bij 4 dB C/N bleek 8 MHz IF meer complete velden op te
leveren dan 18 MHz IF. De tabellen en beelden tonen ook dat vrije sync nog
herkenbare letters kan geven nadat automatische sync wegvalt. Dit is een
simulatieresultaat, geen gemeten ontvangergevoeligheid op RF.

## Ontwerp en huidige grenzen

```text
UHD C / Soapy C / libiio C / IQ-bestand / FM-demo
  ├─ ruwe IQ → Hann FFT + niveaumeting → spectrum
  └─ complex IF-filter → FM-demodulatie → composietsignaal
       ├─ video-de-emphasis → video/sync → raster → grijsbeeld of PAL-kleur
       ├─ 6 MHz FM-mono of 7 MHz FM-stereopaar → 48 kHz → SDL-audio
       └─ 6,552 MHz NICAM → decoderthread → 32 kHz → 48 kHz → SDL-audio
```

Een workerthread leest IQ en verwerkt DSP; SDL draait in de hoofdthread.
Uitlijning en frame stacking draaien in een aparte thread en houden de
ontvangstthread of het schermslot niet vast tijdens de berekening. Bij live SDR
staat maximaal één nieuw beeld voor stacking klaar. Als de stack achterloopt,
vervangt het nieuwste beeld het wachtende beeld. **SKIP** telt zulke overgeslagen
stackbeelden; **DROPS** telt onderbrekingen van de SDR-datastroom. De liveweergave
blijft bijgewerkt. Bestanden en demo mogen op de stack wachten om alle beelden
in volgorde te verwerken. CPU-belasting en USB-doorvoer kunnen nog steeds de
realtime ontvangst begrenzen.

Met `--rx-block N` stel je het aantal IQ-samples per leesaanroep in
(2048–1048576, standaard 16384 voor B210, 131072 voor Pluto en 32768 voor overige bronnen).
Bij 10 MS/s is 16384 ongeveer 1,64 ms en 32768 ongeveer 3,28 ms;
131072 is ongeveer 13,11 ms. Vergelijk bijvoorbeeld 8192, 16384 en 32768 bij
dezelfde samplerate en hetzelfde signaal en let op **DROPS**. Dit is de
applicatieblokgrootte, niet de UHD USB-pakketgrootte of samplerate. Grotere
blokken kunnen aanroepoverhead verminderen, maar vergroten ook de vertraging.
De SDR wordt in een aparte thread uitgelezen, met ongeveer 100 ms IQ-buffer
(minimaal vier, maximaal 256 blokken). De stream start pas wanneer de DSP en
buffer klaar zijn. Korte verwerkingspieken hoeven daardoor het uitlezen niet
meer te onderbreken. Bij een volle buffer worden oude samples verwijderd;
**DROPS** telt ook deze onderbrekingen en de decoder zoekt opnieuw sync.
Bij afsluiten toont `SDR receive` hardware-overflows, de hoogste buffervulling
in milliseconden en `discarded samples` (verlies in de softwarebuffer).
Een blijvende DSP-achterstand of USB-beperking wordt hiermee niet opgelost.
Bij overflows wordt de filter- en decoderhistorie gewist en opnieuw
synchronisatie gezocht. De teller maakt ontvangstproblemen zichtbaar.
Opnieuw afstemmen en wijzigingen van DSP-instellingen starten de decoder
opnieuw; tijdens die overgang kan het laatste beeld kort blijven staan.
Bij de B210 worden afstemcommando's op de verwerkingsthread uitgevoerd,
terwijl de ontvangstthread de stream blijft uitlezen. Samples van tijdens
de overgang worden overgeslagen. Zo blokkeert een afstemcommando het uitlezen
niet en hoeft de stream bij ingedrukt afstemmen niet te stoppen en herstarten.

Het alignment-IF en de FM-demodulatie daarvan draaien in een eigen thread.
Het video-IF en de video-demodulatie worden tegelijk verwerkt. De beeldopbouw
combineert uitsluitend resultaten van hetzelfde IQ-blok, op dezelfde sampleklok
met dezelfde filtervertraging. Resets, afstemmen en polariteitswijzigingen
resetten beide paden. H/V-syncdetectie en rasteropbouw blijven bij elkaar.

Na het verkrijgen van H/V-lock volgt de rasterklok kleine timingcorrecties
geleidelijk, zodat meetruis op de syncpulsen minder beeldtrilling veroorzaakt.
Bij de eerste lock en grote faseverschuivingen wordt snel opnieuw uitgelijnd.
Het vrije livebeeld blijft onafhankelijk van deze regeling rollen.

Bij directe B210/HackRF/Pluto-ontvangst is frame stacking beschikbaar tot en met
**10 MS/s**. Boven die werkelijke samplerate wordt `--stack-frames` uitgeschakeld met een melding; de S-toets en
stackknoppen zijn dan geblokkeerd. De normale H/V-synchronisatie blijft actief.
Het ongebruikte alignmentbeeld wordt dan niet opgebouwd. Bij free sync wordt
alleen het zichtbare livebeeld opgebouwd; met free sync uit het gesynchroniseerde
beeld. Zo wordt geen tweede, ongebruikt beeld berekend.
Bij IQ-bestanden en demo geldt deze beperking niet: stacking en beide rasters
blijven ook boven 10 MS/s beschikbaar. Afspelen mag op de verwerking wachten.

`make bench` meet de verwerking van synthetische IQ bij 10, 12 en 20 MS/s, met
blokken van 16384 samples, video-IF 8 MHz en alignment-IF 3,2 MHz. De seriële
referentie bouwt beide rasters op; de parallelle versie gebruikt bovenstaande
samplerate-afhankelijke instellingen. De tijden zijn seconden verwerking per
seconde IQ, exclusief USB, tekenen en stacking. `wide`, `wait` en `raster` tonen
respectievelijk video-IF/FM, wachten op alignment, en sync/beeldopbouw. Controleer
bij echte ontvangst altijd ook `discarded samples`; deze benchmark garandeert
geen verliesvrije ontvangst op elke machine.

PAL-kleur wordt automatisch geprobeerd bij **video-IF >= 10 MHz** en
**samplerate >= 12 MS/s**, ook na wijzigen van de IF met `[` en `]`. Er is geen
aparte kleurinstelling in de ontvanger. De alignment-IF blijft onafhankelijk
op 3,2 MHz staan. Bij een smallere video-IF blijft het bestaande monochrome pad
actief. FM-mono en NICAM vereisen minimaal 14 MS/s en 14 MHz video-IF;
de FM-stereoparen vereisen minimaal 16 MS/s en 16 MHz video-IF.
Deze geluidsdecoders werken op Linux en Windows, onafhankelijk van kleur-lock.

Het brede videopad bewaart de 4,43361875 MHz chroma. De kleurdecoder meet de
wisselende kleurburst per lijn en middelt de twee lijnfasen afzonderlijk over
meerdere lijnen. Een burst-PLL volgt kleine kleurfrequentieafwijkingen, zodat
de middeling geen blijvende kleurfout door klokverschil introduceert. De
herkenning gebruikt de fasecoherentie en PAL-wisseling, niet het aandeel van
de burst in alle breedbandruis van één lijn. Hij bepaalt daaruit de U/V-fase en PAL V-wisseling,
scheidt luminantie en chroma en vormt RGB. De status toont `PAL COLOR LOCK` of
`PAL AUTO: SEEK BURST / MONO`. Zonder geldige burst schakelt hij terug naar
zwart-wit. Korte onzekere bursts worden maximaal 0,8 ms overbrugd met de
voorspelde fase en lijnwisseling, om zwart-witstrepen te beperken. F blijft de onafhankelijke vrijlopende weergave kiezen; de burst
wordt altijd op de teruggewonnen lijnklok gezocht. Bestandsstacking blijft
beschikbaar; daarbij wordt luminantie gestapeld, terwijl het livebeeld RGB houdt.

Voor een eerste live kleurtest kun je het bestaande 20 MS/s-commando gebruiken.
Een video-IF van 10 MHz kan de FM-kleurzijbanden merkbaar verzwakken: probeer
bij voldoende signaal 16 of 18 MHz video-IF met 18 MHz SDR-bandbreedte. De
benodigde ruimte hangt ook van de FM-deviatie van de zender af. Dit is een
eerste PAL 4.43-decoder met een chromanotch en quadratuurdemodulatie, zonder
adaptief kamfilter of PAL 1H-vertragingslijn; fijne details kunnen nog
kleurartefacten geven. De live kwaliteit moet met een echte kleurenbron
worden beoordeeld.

Een reproduceerbaar testbestand met 75% RGB-kleurbalken:

```bash
./build/atv-tx --output build/pal-bars.cf32 --color --seconds 2
./build/atv-rx --file build/pal-bars.cf32 --loop --if-bandwidth 18000000
```

De zenderoptie `--color` kiest het testpatroon; de ontvanger detecteert kleur
automatisch. Zonder die zenderoptie blijft het bestaande ATV 123-patroon gelden.
Timing, burstfase en kleurverschilcoëfficiënten volgen
[ITU-R BT.470](https://www.itu.int/rec/R-REC-BT.470/en).
De burst-PLL en het vertraagd uitschakelen van kleur sluiten aan bij principes
in klassieke ontvangers zoals de
[Philips TDA8362](https://ksp-electronics.com/media/60309/tda8361_8362.pdf).
Dit is geen volledige emulatie van die chip: automatische kleurversterking
(ACC) en de PAL 1H-vertragingslijn zijn nog niet toegevoegd.

Video-IF/FM en alignment-IF/FM werken in afzonderlijke threads. Terwijl de
beeldthread blok N verwerkt, kunnen beide filters blok N+1 verwerken. Beide
resultaten worden steeds als één paar op de oorspronkelijke sampleklok
verwerkt. EOF en bestandslussen verwerken het laatste blok eerst; resets
wissen beide paden en de kleurreferentie. `make bench` toont ook de kleurroute
zonder en met deze overlap. De pipelinecijfers zijn totale tijden; de losse
`wide/wait/raster`-tellers gelden alleen voor de niet-gepipelinede metingen.

De automatische tests controleren de volledige IF/FM/PAL-keten op 20 en
24 MS/s, omgekeerde polariteit, grijsniveaus, dambord, filtercontinuïteit bij
willekeurige blokgrenzen, doorlaat/stopband en vrije sync zonder syncpulsen.
De SDL-demo is zonder scherm getest en haalt hier 50 velden/s op 20 MS/s.
Deze automatische tests bewijzen geen verliesvrije hardwareontvangst. De AWGN-test
onderzoekt lage C/N in simulatie; een gekalibreerde RF-gevoeligheidsmeting ontbreekt. De haalbare doorvoer hangt ook van USB,
driver en CPU af.

Handige diagnose:

```sh
./build/atv-rx --help
SDL_VIDEODRIVER=dummy ./build/atv-rx --demo --seconds 2 --snapshot build/demo.bmp
```

## Mono FM-geluid op 6 MHz

Bij **samplerate >= 14 MS/s en video-IF >= 14 MHz** probeert de ontvanger
de 6 MHz FM-geluidsdraaggolf uit het composietsignaal te halen wanneer
6 MHz mono als audiobron is geselecteerd (de standaard bij een nieuwe configuratie).
Dit werkt op Linux en Windows, onafhankelijk van kleur-lock en free-sync,
zowel met een SDR als bij IQ-bestanden. Er is geen aparte inschakeloptie nodig. De contestinstellingen
met een smalle IF activeren deze audioketen niet.

De mono-decoder ondersteunt één draaggolf met **±50 kHz deviatie**
als uitgangspunt voor het niveau en **50 µs de-emphasis**. Dit volgt de
analoge tv-parameters van [ITU-R BT.470](https://www.itu.int/rec/R-REC-BT.470/en).
ATV-zenders kunnen anders zijn ingesteld; controleer de zender bij een
verkeerd volume of afwijkende klank. Deze FM-keten is mono; NICAM-stereo is apart selecteerbaar (zie hieronder).

De audio wordt na de brede video-IF/FM-demodulator verwerkt, op dezelfde
worker terwijl de hoofdthread het beeld opbouwt. Een smal kanaalfilter,
tweede FM-demodulator, audio-laagdoorlaat, DC-filter en de-emphasis leveren
48 kHz mono, afgespeeld op beide kanalen van de gedeelde SDL-stereo-uitgang.
Op Linux wordt standaard de systeemuitgang gebruikt; de Windows-wizard biedt
ook een keuze uit de beschikbare geluidsapparaten. De FM-audio heeft geen
automatische squelch: ook zonder carrier-lock hoor je ruis en zwakke signalen. **A** dempt het geluid; het volume regel je via het
besturingssysteem. De UI toont `AUDIO 6M: LOCK`, `SEEK CARRIER` of `MUTED`.
Als geen geluidsapparaat beschikbaar is, blijft video gewoon werken.

De afspeelbuffer is begrensd op de actuele reserve plus 120 ms; bij een overschrijding wordt
oude audio weggegooid om oplopende vertraging te voorkomen. De afsluitmelding
rapporteert het aantal van deze resets. Afstemmen, IF-wijzigingen en
streamonderbrekingen wissen filterhistorie en oude audio. Er is nog geen
aparte regeling die audio en video exact lipsynchroon houdt.

Een uitgangspunt voor de live test met de B210 is de werkende kleurinstelling:

```sh
./build/atv-rx --b210 --ref internal --antenna TX/RX --channel 0 \
  --frequency 2335000000 --rate 20000000 --bandwidth 18000000 \
  --if-bandwidth 16000000 --gain 60 --rx-block 16384
```

De audiotests controleren een bekende FM-toon, een draaggolfafwijking,
video/chroma-interferentie, hoorbare ruis zonder lock, draaggolfverlies, blokgrenzen,
verschillende samplerates, de volledige IQ/IF/FM-keten en SDL-bufferresets.
`make bench` bevat ook de combinatie PAL-kleur plus audio.

## FM-stereo op 7,02/7,22 en 7,38/7,56 MHz

Met **N** wissel je tussen 6 MHz mono → NICAM → FM-stereo op **7,02/7,22 MHz**
→ FM-stereo op **7,38/7,56 MHz** → 6 MHz mono.
Beide stereostanden zijn beschikbaar bij **samplerate >= 16 MS/s en video-IF >= 16 MHz**;
bij een smallere IF worden deze standen in de keuzecyclus overgeslagen.
20 MS/s met 16 MHz IF is een bruikbaar uitgangspunt. Bij versmallen van de IF
terwijl stereo geselecteerd is, wordt de audio stil tot de IF weer breed genoeg is.

De voorlopige kanaaltoewijzing is **7,02 MHz links, 7,22 MHz rechts**,
of **7,38 MHz links, 7,56 MHz rechts** voor het tweede paar.
Met **Shift+N** wissel je die om; de UI toont `L/R` of `R/L`.
De toewijzing van de ontvangen zender is hiermee op het gehoor te controleren.
De frequentie is precies 7,22 MHz, niet de ook gebruikte 7,20 MHz.
In deze stand zijn de 6 MHz-decoder en NICAM uitgeschakeld.
Beide FM-kanalen hebben een eigen kanaalfilter, 50 µs de-emphasis
en een gedeelde 48 kHz stereoklok. `LOCK` betekent dat beide carriers zijn
gevonden. De lock-indicator dempt de audio niet: zonder carrier hoor je ruis.
Er wordt geen Panda/Wegener-compander toegepast.

## NICAM op 6,552 MHz en geluidsbron kiezen

De zender mag **FM op 6 MHz en NICAM op 6,552 MHz tegelijk** meesturen in het
composietsignaal. Bij samplerate en video-IF vanaf 14 MHz draait standaard
alleen de FM-audiodecoder, met daarnaast een spectrale NICAM-indicator.
Voor NICAM is **20 MS/s met 16 MHz video-IF** het aanbevolen uitgangspunt.

- `NICAM --`: geen duidelijke energie in het detectiekanaal.
- `NICAM DETECT`: energie rond 6,552 MHz; dit kan NICAM zijn, maar ook ruis of
  storing. Dit is **geen bevestigde digitale synchronisatie**.
- **N** kiest NICAM als geluidsbron en start de volledige decoder. `NICAM SEEK`
  betekent dat deze nog synchronisatie zoekt; `NICAM LOCK` vereist meerdere
  geldige frames met een ondersteunde audiomodus en voldoende correcte pariteit.
- Nogmaals **N** stopt de NICAM-decoder en kiest FM-stereo bij voldoende
  bandbreedte, anders 6 MHz mono. Tijdens
  NICAM-weergave wordt de FM-audio niet gedecodeerd om rekentijd te besparen.
- **A** dempt de gekozen bron. Er wordt niet automatisch tussen bronnen gewisseld.
  NICAM blijft stil bij ontbrekende synchronisatie; slechte audioframes worden
  met stilte vervangen. De decoder ondersteunt momenteel de stereomodus.

Met **`--nicam`** kun je direct met NICAM als geluidsbron starten. Zonder opgeslagen
audiokeuze of deze optie is 6 MHz FM-mono de standaard. Bij onvoldoende samplerate of video-IF wordt
`--nicam` geweigerd. Bronwisseling, afstemmen en streamonderbrekingen wissen
oude audio en decoderhistorie. Een smalle IF schakelt de audiopaden uit.

De NICAM-keten selecteert 6,552 MHz na de primaire video-FM-demodulator,
filtert en resamplet naar 1,456 MS/s complex IQ. De bestaande C-decoder uit
het eigen project [pe1itr/nicam](https://github.com/pe1itr/nicam) draait in een eigen thread en levert 32 kHz stereo met
J.17-de-emphasis. SDL converteert dit naar de gedeelde 48 kHz stereo-uitgang;
FM-mono wordt naar beide speakers gestuurd. Er is geen extern proces of
extra SDR nodig. De overgenomen Apache-2.0-bron, herkomst en lokale aanpassingen
staan in `src/nicam_core/`.

De fase- en timingregeling gebruikt rustigere instellingen dan de oorspronkelijke
decoder (Costas-alpha 0,04; timing-gain 0,05). Dit vermindert het volgen van ruis
rond de ontvangstgrens, zonder de framevalidatie te versoepelen. Een vergelijking
met toegevoegde ruis aan de uitgefilterde tweede live-opname leverde meer geldige
frames op; dit is geen gekalibreerde meting van de RF-gevoeligheid.
`build/test-nicam-sensitivity` vergelijkt beide instellingen op onafhankelijk
gegenereerde IQ met vaste ruis en controleert acquisitie bij ±3 kHz en +50 ppm.

De NICAM-IQ-wachtrij is begrensd op 250 ms. Bij overloop stopt de decoder
zonder de video-ontvangst te blokkeren; de afsluitmelding meldt de overflows.
Schakel dan met N terug naar FM en opnieuw naar NICAM om opnieuw te starten.
Test tijdens live ontvangst ook de SDR-bufferstatistieken: de softwarebenchmark
omvat geen USB-overdracht of het tekenen van het venster.

`make test` controleert onafhankelijk gegenereerde NICAM-stereotonen,
NICAM en FM tegelijk in de volledige IQ/IF/FM-keten, onderdrukking van de
naburige FM-draaggolf in de detector, bronwisseling, ruis, reset en begrensde
wachtrijen. `make bench` meet detectie en volledige NICAM-verwerking apart.

### PAL-video-de-emphasis

De ontvanger past standaard **ITU-R F.405-1 voor 625 lijnen** toe op het
videopad. Dit herstelt de verhouding tussen helderheid en kleur bij onder meer
Digital Baseband van FM-ATV Projects. De correctie heeft een lineaire fase,
passend bij hun digitale pre-emphasis. Een analoge pre-emphasis met een andere
fasekarakteristiek is hiermee niet afzonderlijk getest.

Gebruik **`--video-deemphasis off`** voor vlakke signalen zonder pre-emphasis,
zoals de huidige `atv-tx`-testsignalen en daarmee gemaakte scatter-opnamen.
`--video-deemphasis pal` kiest expliciet de standaard. Dit geldt voor zowel
bestanden als live SDR-ontvangst, ook bij zwart-witweergave.

De correctie draait met FFT-convolutie in de videothread, na de aftakking naar
FM/NICAM-audio. Het afzonderlijke syncsignaal krijgt dezelfde extra vertraging
van 128 samples. Afstemmen en streamonderbrekingen wissen de filterhistorie.
Het ingestelde `--deviation` blijft bepalend voor de totale videoschaal.

Bronnen: [ITU-R F.405-1](https://www.itu.int/dms_pubrec/itu-r/rec/f/R-REC-F.405-1-197007-W%21%21PDF-E.pdf),
[Digital Baseband verwerking](https://fm-atv.nl/developer/) en
[filtermetingen](https://fm-atv.nl/video-measurements/).
`make test` controleert onafhankelijk in het frequentiedomein voorbewerkt
PAL-beeld op 12, 20 en 24 MS/s, inclusief kleuren, blokgrenzen en reset.

De toetsen **`[` en `]`** verlagen/verhogen de digitale IF-bandbreedte met
**1 MHz per druk**, begrensd op 250 kHz en de werkelijke samplerate.
Voor kleur met FM/NICAM kun je **18 MS/s met 16 MHz IF** gebruiken
(`--rate 18000000 --if-bandwidth 16000000`). Dit verwerkt 10% minder IQ-samples
per seconde dan 20 MS/s; de totale CPU-winst hangt ook af van de filters,
resampling en weergave. Controleer bij live ontvangst de werkelijk ingestelde
samplerate en de tellers voor verloren samples en streamonderbrekingen.

De gedeelde FM/NICAM-audio-uitgang vult vóór afspelen 80 ms audio, om korte
verwerkings- en schedulingpieken op te vangen. Bij een lege lopende SDL-wachtrij
pauzeert de uitvoer en groeit de reserve met 40 ms, tot maximaal 240 ms.
Afstemmen en bronwissels behouden de opgebouwde reserve. De afsluitregel
`playback buffer refills` telt die gebeurtenissen; dit is een aanwijzing voor
bufferleegloop, geen directe meting van hoorbare uitval in de geluidskaart.
`playback queue resets` telt het leegmaken boven de actuele reserve plus 120 ms
(dus aanvankelijk 200 ms, maximaal 360 ms). Het log vermeldt ook de gekozen
reserve, de grootste pauze tussen audiopakketten en de gemiddelde aanvoer
in frames/s (streefwaarde 48000). Bewuste resets tellen niet mee als aanvoerpauze.
Deze buffering kan langdurige overbelasting of verlies van IQ niet herstellen.

### Converterfrequentie weergeven

Met `--frequency HZ` geef je de gewenste ontvangstfrequentie vóór de converter
op. De ontvanger trekt `--lo HZ` daarvan af voor de SDR-afstemming. Het scherm
en de spectrumschaal tonen de ontvangstfrequentie vóór de converter. Bijvoorbeeld:

```bash
./build/atv-rx --hackrf --frequency 2335000000 --lo 900000000 \
  --rate 18000000 --bandwidth 18000000 --if-bandwidth 16000000 \
  --gain 49 --rx-block 16384
```

De HackRF stemt hiermee af op **1435 MHz** en het scherm toont **2335 MHz**.
Toetsen, muiswiel en klikken in het spectrum blijven op de normale manier
afstemmen. De LO is standaard nul. De berekende SDR-frequentie moet boven nul
en maximaal 6 GHz zijn; de ontvangstfrequentie vóór de converter mag hoger
liggen. De optie corrigeert geen spectrumsinversie door een converter.

### Screenshot tijdens ontvangst

Druk op **P** om het volledige venster als PNG in de huidige werkmap op te
slaan, inclusief beeld, spectrum en status. De bestandsnaam bevat de weergegeven
ontvangstfrequentie (dus inclusief converter-LO) en UTC-tijd, bijvoorbeeld
`fm-atv-2335000000Hz-20261017T153045-123Z-000.png`. Het laatste nummer voorkomt
overschrijven bij meerdere opnamen met dezelfde timestamp. De terminal meldt
het opgeslagen bestand of een fout; ontvangst blijft doorlopen.
Stacking pauzeren/hervatten kan met **Shift+P** of de bestaande schermknop.
De bestaande optie `--snapshot PATH` voor een BMP bij afsluiten blijft beschikbaar.

### HackRF: vaste VGA, LNA via de gainknop

Bij HackRF regelt omhoog/omlaag alleen RX LNA: 0..40 dB in stappen van 8 dB.
Het hoofdscherm toont daarom `LNA`. RX VGA staat standaard vast op **28 dB**;
de RF AMP blijft aan (nominaal 14 dB), onafhankelijk van de LNA-knop.
De automatische verdeling van totale gain door SoapyHackRF wordt niet gebruikt.
Andere SDR's houden hun bestaande gainregeling.

De vaste VGA kan via `hackrf_vga=28` in de configuratie of `--hackrf-vga 28`
worden ingesteld (0..62 dB, stappen van 2). Er is geen extra schermbediening.
Het infoscherm toont VGA en AMP. De meetstatus bevat bij HackRF
`receiver.gain_control="lna"`, `rx_lna_db`, `rx_vga_db` en `rf_amp_db`;
`gain_db` en `requested_gain_db` betekenen nu LNA, niet totale gain.
`--gain` en opgeslagen `gain` krijgen dezelfde betekenis. Oude waarden boven
40 worden begrensd tot 40; andere waarden worden afgerond op 8 dB. Met W sla je
de nieuwe instellingen op. Oude en nieuwe gainwaarden zijn dus niet rechtstreeks
vergelijkbaar. Controleer na herstart clipping, Video-SNR en NICAM: 28 dB is een
gekozen uitgangspunt voor de vergelijking, geen gemeten optimale instelling.
