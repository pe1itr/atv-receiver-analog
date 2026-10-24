# Installeren en compileren

Deze handleiding beschrijft de huidige bouwroute op **Fedora 43**: een native
Linux-build en een Windows x64-cross-build met MinGW-w64. Voer de commando's
uit in de hoofdmap van de broncode, waar `Makefile` staat. Download en pak
hiervoor de broncode uit of gebruik een Git-checkout van de gewenste versie.
Voor een Windows-release gebruik je de broncode van de bijbehorende release-tag.

De pakketnamen hieronder zijn afgestemd op Fedora. Andere Linux-distributies
kunnen de C-code ook bouwen met equivalente ontwikkelpakketten; de
Windows-verpakkingsscripts gebruiken specifiek Fedora-paden en RPM-pakketten.

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

```sh
sudo dnf install gcc make pkgconf-pkg-config python3 \
  sdl2-compat-devel fftw-devel SoapySDR-devel uhd-devel \
  libpng-devel freetype-devel fontconfig-devel \
  google-noto-sans-mono-vf-fonts
```

Fedora 43 levert de SDL2-API via `sdl2-compat-devel`. De applicatie gebruikt
SDL2, geen rechtstreeks op SDL3 gerichte API. UHD en SoapySDR zijn in de
huidige Linux-Makefile verplichte bouwdependencies, ook voor gebruik met de demo.
Python 3 is nodig voor de integratietests.

Voor optionele PlutoSDR-ondersteuning:

```sh
sudo dnf install libiio-devel
pkg-config --modversion libiio
```

De code gebruikt **libiio >= 0.23 en < 1.0**. De Makefile detecteert die
versiereeks automatisch; met libiio 1.x wordt Pluto-ondersteuning niet gebouwd.

Controleer de verplichte dependencies:

```sh
pkg-config --modversion sdl2 fftw3f SoapySDR uhd libpng freetype2 fontconfig
```

Bij een ontbrekend pakket eerst de installatie of `PKG_CONFIG_PATH` corrigeren.

### 2. Compileren en starten

```sh
make -j2 all
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

- **`Package ... was not found`:** controleer de ontwikkelpakketten en het
  resultaat van `pkg-config --modversion` hierboven.
- **Pluto-driver ontbreekt:** controleer de libiio-versie en bouw opnieuw met
  `make -B all` nadat libiio >= 0.23 en < 1.0 is geïnstalleerd.
- **Windows-verpakking mist een font of licentiebestand:** controleer of alle
  genoemde Fedora-pakketten zijn geïnstalleerd. Het script gebruikt hun vaste paden.
- **Nieuwe code lijkt niet actief:** start het zojuist gebouwde bestand en
  controleer `--version`; een eerder naar `~/.local/bin` gekopieerd bestand
  wordt niet automatisch bijgewerkt door `make`.

Bediening, ontvangstinstellingen en de bandbreedtegrenzen voor kleur en geluid
staan in de [README](README.md).
