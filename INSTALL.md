# Installeren en compileren

De Linux-ontvanger kan worden gecompileerd op **Debian, Ubuntu, Linux Mint
(ook LMDE) en Fedora**. Je hoeft hiervoor niet van distributie te wisselen.
De Windows x64-cross-build met MinGW-w64 gebruikt specifiek Fedora.
Voer de commando's
uit in de hoofdmap van de broncode, waar `Makefile` staat. Download en pak
hiervoor de broncode uit of gebruik een Git-checkout van de gewenste versie.
Voor een Windows-release gebruik je de broncode van de bijbehorende release-tag.

Kies hieronder de pakketcommando's voor jouw distributie. De native Linux-build
gebruikt `pkg-config`, zodat ook de librarypaden van Debian/Ubuntu/Mint worden
gevonden. De Windows-verpakkingsscripts gebruiken Fedora-paden en RPM-pakketten.

## Windows gebruiken zonder compileren

1. Pak de volledige Windows x64-ZIP uit naar een gewone map.
2. Installeer zo nodig de USB-driver volgens `LEESMIJ.txt` in de ZIP.
3. Start `atv-rx.exe`. Kies **W + Enter** voor de instellingen en daarna
   **2** voor HackRF of **3** voor PlutoSDR.
4. Stel de ontvangst en geluidsuitgang in. Druk tijdens ontvangst op **W**
   om de instellingen te bewaren.

Laat de DLL's, `modules` en `font.ttf` bij het programma staan. De doelplatforms
zijn Windows 10 vanaf 1903 en Windows 11, x64. B210/UHD is niet meegebouwd.
SDRplay vereist extra installatie; zie de [README](README.md#sdrplay-rsp1b-experimental).

## Linux compileren

### 1. Ontwikkelpakketten installeren

Open een terminal in de uitgepakte bronmap met `Makefile`.
Gebruik uitsluitend het blok voor jouw distributie.

**Debian / Ubuntu / Linux Mint / LMDE:**

```sh
sudo apt update
sudo apt install build-essential pkg-config \
  libsdl2-dev libfftw3-dev libsoapysdr-dev libuhd-dev \
  libpng-dev libfreetype6-dev libfontconfig1-dev fonts-dejavu-core
```

**Fedora** (pakketnamen gecontroleerd op Fedora 43):

```sh
sudo dnf install gcc make pkgconf-pkg-config python3 \
  sdl2-compat-devel fftw-devel SoapySDR-devel uhd-devel \
  libpng-devel freetype-devel fontconfig-devel \
  google-noto-sans-mono-vf-fonts
```

Fedora 43 levert de SDL2-API via `sdl2-compat-devel`. De applicatie gebruikt
SDL2, geen rechtstreeks op SDL3 gerichte API. UHD en SoapySDR zijn in de
huidige Linux-Makefile verplichte bouwdependencies, ook voor gebruik met de demo.
Python 3 is nodig voor de optionele lokale integratietests en de statusclient,
maar niet om de Linux-ontvanger te compileren. Installeer het zo nodig met
`sudo apt install python3` of `sudo dnf install python3`.

Voor optionele **PlutoSDR-ondersteuning**, op Debian/Ubuntu/Mint/LMDE:

```sh
sudo apt install libiio-dev
pkg-config --modversion libiio
```

Op Fedora:

```sh
sudo dnf install libiio-devel
pkg-config --modversion libiio
```

De code gebruikt **libiio >= 0.23 en < 1.0**. De Makefile detecteert die
versiereeks automatisch; met libiio 1.x wordt Pluto-ondersteuning niet gebouwd.
Ter referentie: de officiële pakketlijsten vermelden libiio 0.26 voor
[Debian 13](https://packages.debian.org/trixie/libiio-dev) en 0.25 voor
[Ubuntu 24.04](https://packages.ubuntu.com/noble/libiio-dev). Controleer altijd
de werkelijk geïnstalleerde versie; andere releases of eigen repositories
kunnen een andere versie leveren.

Controleer de verplichte dependencies:

```sh
pkg-config --modversion sdl2 fftw3f SoapySDR uhd libpng freetype2 fontconfig
```

Of voer `make check-deps` uit: dat controleert de dependencies en toont bij
ontbrekende pakketten het installatiecommando voor jouw distributie.
Ontbrekende of ongeschikte libiio geeft een waarschuwing; andere ontbrekende
bouwdependencies stoppen de build. `HAVE_LIBIIO=1` met een ongeschikte libiio
stopt ook, zodat Pluto-ondersteuning niet ongemerkt ontbreekt.

### 2. Compileren en starten

```sh
bash scripts/build-linux.sh
./build/atv-rx --version
./build/atv-rx --no-config --no-wizard --demo
```

Dit bouwt alleen de ontvanger: `build/atv-rx`. De offline hulpmiddelen zijn
optioneel en worden afzonderlijk gebouwd met `make tools`:

| Bestand | Functie |
| --- | --- |
| `build/atv-rx` | Ontvanger met beeld en geluid |
| `build/atv-tx` (`make tools`) | Offline generator van test-IQ; zendt zelf niets uit |
| `build/atv-eval` (`make tools`) | Offline evaluatiehulpmiddel |

De demo heeft geen aangesloten SDR nodig. Sluit het venster met **Esc**.
Start daarna `./build/atv-rx` voor de configuratiewizard en live ontvangst.

Het bouwscript controleert eerst de ontwikkelpakketten, toont waarschuwingen
en fouten en bewaart de uitvoer in `build/linux-build.log`. Bij een fout stopt
het met een verwijzing naar dat log en deze handleiding. Het installeert niets
automatisch. Direct `make -j2 all` gebruiken kan ook; de dependencycontrole
wordt dan eveneens uitgevoerd. Bouw zonder `sudo`.

Na het toevoegen van libiio, of bij gewijzigde bouwopties, forceer je een
nieuwe compilatie. Alleen `make` detecteert niet alle wijzigingen in de
beschikbare dependencies of compileropties:

```sh
make -B -j2 all
```

Expliciet zonder PlutoSDR-ondersteuning bouwen:

```sh
make -B -j2 HAVE_LIBIIO=0 all
```

Gebruik dezelfde `HAVE_LIBIIO=0`-optie bij daaropvolgende testcommando's als je
deze variant wilt blijven gebruiken.

### 3. Testen (alleen met lokale testbroncode)

De map `tests/` wordt niet meegeleverd op GitHub. Onderstaande test- en
benchmarkcommando’s zijn alleen beschikbaar in de lokale ontwikkelomgeving
met die map. Voor een controle zonder testbroncode gebruik je de demo hierboven.

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy make test
```

Deze tests gebruiken synthetische signalen en gesimuleerde hardwareaanroepen;
ze vereisen geen aangesloten SDR. De dummy-drivers maken uitvoering zonder
scherm en geluidskaart mogelijk. Een geslaagde test vervangt geen live
ontvangsttest met je eigen SDR.

Optioneel de verwerkingssnelheid meten:

```sh
make bench
```

### 4. Hardware en installatiepad

- **HackRF:** installeer daarnaast de SoapyHackRF-module voor je
  SoapySDR-installatie en de bijbehorende USB-toegang/udev-regels.
  Controleer de detectie met `SoapySDRUtil --find="driver=hackrf"` als dit
  hulpprogramma is geïnstalleerd. De Linux-Makefile bouwt de module niet zelf.
  Op Debian/Ubuntu/Mint/LMDE: `sudo apt install soapysdr-tools soapysdr-module-hackrf hackrf`.
- **PlutoSDR:** bouw met de ondersteunde libiio-versie en configureer USB-toegang
  of een netwerk-URI. Zie de [Pluto-instructies](README.md#plutosdr--ad9361).
- **B210:** geef de passende firmware-/FPGA-map op met
  `--uhd-images-dir /pad/naar/images`. Standaard zoekt de ontvanger in
  `uhd-images/` ten opzichte van de huidige werkmap. Voor de beschreven Kintex-7-kloon zijn
  andere images nodig dan voor een standaard Ettus B210.
- **SDRplay:** installeer de officiële API en SoapySDRPlay3 apart;
  zie de [SDRplay-instructies](README.md#sdrplay-rsp1b-experimental).

Er is geen `make install`-doel. Je kunt het programma vanuit `build/` blijven
starten. Optioneel installeer je alleen de ontvanger in je eigen gebruikersmap:

```sh
install -Dm755 build/atv-rx "$HOME/.local/bin/atv-rx"
"$HOME/.local/bin/atv-rx" --version
```

De gedeelde libraries en drivers moeten op het systeem beschikbaar blijven.
Instellingen staan op Linux in `~/.atv-rx.conf`.

## Windows x64 compileren op Fedora

Dit is een **cross-build op Linux**. De repository bevat geen afzonderlijke
bouwroute voor MSVC of compileren vanuit een Windows-opdrachtprompt.
De scripts downloaden vastgelegde bron-/SDL-pakketten en controleren hun
SHA-256-checksums. Voor de eerste build is internettoegang nodig.
De bouwroute controleert vóór het downloaden de Fedora-paden, gereedschappen,
verpakkingspakketten en Python-versie. Een fout meldt de bouwfase en verwijst
naar deze instructies. Compilerwaarschuwingen blijven zichtbaar in de terminal.

### 1. Bouw- en verpakkingsdependencies installeren

```sh
sudo dnf install make cmake pkgconf-pkg-config curl python3 tar gzip \
  mingw64-gcc mingw64-gcc-c++ mingw64-binutils \
  mingw64-fftw mingw64-freetype mingw64-libpng mingw64-libxml2 \
  mingw64-libusb1 mingw64-bzip2 mingw64-zlib mingw64-win-iconv \
  mingw64-winpthreads mingw64-libstdc++ \
  freetype google-noto-fonts-common google-noto-sans-mono-vf-fonts
```

Gebruik Python 3.12 of nieuwer. De verpakking leest ook licentiebestanden en
RPM-versies uit deze pakketten. De native Linux-ontwikkelpakketten uit de
vorige sectie zijn niet nodig om uitsluitend de Windows-build te maken.

### 2. Bouwen en verpakken

```sh
make windows
```

Dit voert achtereenvolgens het downloaden, controleren, bouwen, verzamelen
van DLL's en maken van de ZIP uit. SDL2 wordt als MinGW-pakket opgehaald;
SoapySDR, SoapyHackRF, libhackrf, libiio en FFTW worden vanuit de vastgelegde
bronnen gebouwd. De build gebruikt intern maximaal acht parallelle bouwtaken.

Uitvoer:

| Pad | Inhoud |
| --- | --- |
| `build/windows/atv-receiver-windows-x64.zip` | Uitpakbaar Windows-pakket |
| `build/windows/atv-receiver-windows-x64.zip.sha256` | SHA-256 van de ZIP |
| `build/windows/atv-receiver-windows-x64/` | Uitgepakte distributiemap |
| `build/windows/BUILD-INFO.txt` | Bouwinformatie; momenteel buiten de ZIP |

De Windows-build bevat de ontvanger met PlutoSDR- en HackRF-ondersteuning,
beeld en geluid. `atv-tx`, `atv-eval`, UHD/B210 en SoapySDRPlay3 worden niet
meegeleverd. De verpakking stopt als een benodigde DLL niet gevonden wordt.

### 3. Windows-tests bouwen en uitvoeren (alleen met lokale testbroncode)

Deze stap vereist de lokale map `tests/`, die niet wordt meegeleverd op GitHub.
Voor het bouwen en gebruiken van de Windows-ontvanger is deze stap niet nodig.

Na `make windows`, vanuit de hoofdmap:

```sh
bash scripts/windows/build-tests.sh
```

Dit **bouwt** de testprogramma's onder `build/windows/tests/`; het voert ze
niet uit. Kopieer voor uitvoering op Windows de gewenste test-EXE's naast
`atv-rx.exe` in een kopie van de distributiemap, zodat ze dezelfde DLL's vinden.
Start ze vanuit een opdrachtprompt, bijvoorbeeld:

```bat
test-settings.exe
test-fm-math.exe
test-sound.exe
test-nicam.exe
```

Test daarna de ontvanger op een echte Windows-pc met de gewenste SDR.
De instellingen en het log staan in `%APPDATA%\PE1ITR\ATV Receiver\`.

## Veelvoorkomende bouwproblemen

- **`make: command not found`, `cc: command not found` of `pkg-config` ontbreekt:**
  voer het volledige installatieblok voor jouw distributie uit bij stap 1.
- **`Unable to locate package` bij `apt`:** voer eerst `sudo apt update` uit.
  Controleer in Softwarebronnen of de officiële pakketbronnen voor jouw versie
  actief zijn. Ubuntu heeft voor onder andere SoapySDR de component `universe`
  nodig: activeer die zo nodig met `sudo add-apt-repository universe`, gevolgd
  door `sudo apt update`. Op Mint/LMDE beheer je de bronnen via Softwarebronnen;
  voeg geen Ubuntu-bronnen toe aan Debian of LMDE. Gebruik pakketnamen uit het
  juiste blok: Fedora's `*-devel`-namen werken niet bij `apt`.
  De officiële [Ubuntu-pakketpagina](https://packages.ubuntu.com/noble/libsoapysdr-dev)
  vermeldt welke repositorycomponent het pakket levert.
- **`Package ... was not found`:** controleer de ontwikkelpakketten en het
  resultaat van `make check-deps`. Alleen de runtime-library installeren is
  onvoldoende: de `-dev`- of `-devel`-pakketten leveren headers en `.pc`-bestanden.
  Bij een eigen installatieprefix moet `PKG_CONFIG_PATH` naar de bijbehorende
  `lib/pkgconfig`-map wijzen; stel geen Fedora- of Windows-librarypaden in op
  Debian/Ubuntu/Mint.
- **Compiler- of linkerfout na een geslaagde dependencycontrole:** zoek de eerste
  fout in `build/linux-build.log`; latere fouten kunnen daarvan het gevolg zijn.
  Na installatie van ontbrekende pakketten: `bash scripts/build-linux.sh -B`.
  Vermeld bij een hulpvraag je distributie/versie (`cat /etc/os-release`) en die
  eerste fout met de omliggende regels. Deel geen oud uitvoerbaar bestand als
  bewijs dat de nieuwe compilatie gelukt is.
- **`Killed` of geheugen tekort tijdens compilatie:** probeer `bash scripts/build-linux.sh -j1`.
- **De demo werkt, maar de SDR wordt niet gevonden:** controleer de aparte
  hardwaremodule, USB-toegang en de hardware-instructies hierboven. Compileren
  installeert geen USB-regels of SDRplay-API.
- **Pluto-driver ontbreekt:** controleer de libiio-versie en bouw opnieuw met
  `make -B all` nadat libiio >= 0.23 en < 1.0 is geïnstalleerd.
- **Windows-verpakking mist een font of licentiebestand:** controleer of alle
  genoemde Fedora-pakketten zijn geïnstalleerd. Het script gebruikt hun vaste paden.
  `make windows` is geen native Linux-build voor Debian/Ubuntu/Mint; gebruik daar
  `bash scripts/build-linux.sh`. Voor een Windows-pakket gebruik je de beschreven
  Fedora-bouwomgeving of de kant-en-klare release-ZIP.
- **Nieuwe code lijkt niet actief:** start het zojuist gebouwde bestand en
  controleer `--version`; een eerder naar `~/.local/bin` gekopieerd bestand
  wordt niet automatisch bijgewerkt door `make`.

Bediening, ontvangstinstellingen en de bandbreedtegrenzen voor kleur en geluid
staan in de [README](README.md).
