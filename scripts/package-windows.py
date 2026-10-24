#!/usr/bin/env python3
"""Collect the recursive PE dependency closure; reject unknown DLL imports."""
from pathlib import Path
import re, shutil, subprocess
root=Path(__file__).resolve().parents[1]
build=root/'build/windows'
out=build/'atv-receiver-windows-x64'
# Rebuild the package from the receiver, never from leftover DLLs or files.
if not (out/'atv-rx.exe').is_file():
 raise SystemExit('Build atv-rx.exe before packaging')
for path in out.iterdir():
 if path.name=='atv-rx.exe':continue
 if path.is_dir() and not path.is_symlink():shutil.rmtree(path)
 else:path.unlink()
search=[build/'prefix/bin',build/'deps/SDL2-2.32.10/x86_64-w64-mingw32/bin',Path('/usr/x86_64-w64-mingw32/sys-root/mingw/bin')]
index={p.name.lower():p for d in reversed(search) for p in d.glob('*.dll')}
system=set('advapi32.dll bcrypt.dll cfgmgr32.dll comctl32.dll comdlg32.dll crypt32.dll dinput8.dll dwmapi.dll gdi32.dll hid.dll imm32.dll iphlpapi.dll kernel32.dll msvcrt.dll ntdll.dll ole32.dll oleaut32.dll powrprof.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll shlwapi.dll user32.dll userenv.dll usp10.dll version.dll winmm.dll winspool.drv winusb.dll ws2_32.dll wsock32.dll wtsapi32.dll ucrtbase.dll'.split())
(out/'modules').mkdir(exist_ok=True)
shutil.copy2(build/'prefix/lib/SoapySDR/modules0.8/libHackRFSupport.dll',out/'modules')
queue=[out/'atv-rx.exe',out/'modules/libHackRFSupport.dll']; seen=set()
while queue:
 p=queue.pop()
 if p.name.lower() in seen:continue
 seen.add(p.name.lower())
 data=subprocess.check_output(['x86_64-w64-mingw32-objdump','-p',str(p)],text=True)
 for name in re.findall(r'DLL Name: (\S+)',data):
  key=name.lower()
  if key in system or key.startswith(('api-ms-win-','ext-ms-win-')):continue
  if key not in index:raise SystemExit(f'Unresolved DLL: {name} (from {p.name})')
  dest=out/index[key].name
  shutil.copy2(index[key],dest)
  queue.append(dest)
shutil.copy2('/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf',out/'font.ttf')
print('Bundled:',', '.join(sorted(seen)))
# Include notices and build provenance.
licenses=out/'licenses'; licenses.mkdir(exist_ok=True)
nicam_licenses=licenses/'nicam_core'
nicam_licenses.mkdir(exist_ok=True)
for name in ['LICENSE','ORIGIN.txt']:
 shutil.copy2(root/'src/nicam_core'/name,nicam_licenses/name)
for package in ['mingw64-libusb1','mingw64-freetype','mingw64-libpng','mingw64-libxml2','mingw64-gcc','mingw64-winpthreads','google-noto-fonts-common']:
 src=Path('/usr/share/licenses')/package
 shutil.copytree(src,licenses/package,dirs_exist_ok=True)
for package in ['mingw64-fftw','mingw64-bzip2','mingw64-win-iconv']:
 src=Path('/usr/share/doc')/package
 for name in ['COPYING','COPYRIGHT','readme.txt']:
  if (src/name).exists():
   (licenses/package).mkdir(exist_ok=True)
   shutil.copy2(src/name,licenses/package/name)
shutil.copy2('/usr/share/licenses/freetype/FTL.TXT',licenses/'mingw64-freetype/FTL.TXT')
# zlib.h contains zlib's full notice and redistribution terms.
shutil.copy2('/usr/x86_64-w64-mingw32/sys-root/mingw/include/zlib.h',licenses/'zlib.h')
for name,sub,file in [('SDL2','SDL2-2.32.10','LICENSE.txt'),('SoapySDR','SoapySDR-soapy-sdr-0.8.1','LICENSE_1_0.txt'),('SoapyHackRF','SoapyHackRF-soapy-hackrf-0.3.4','LICENSE'),('HackRF','hackrf-2024.02.1','COPYING'),('libiio','libiio-0.26','COPYING.txt'),('FFTW','fftw-3.3.10','COPYING')]:
 shutil.copy2(build/'deps'/sub/file,licenses/(name+'.txt'))
shutil.copy2(root/'scripts/windows/LEESMIJ.txt',out/'LEESMIJ.txt')
packages=['mingw64-gcc','mingw64-fftw','mingw64-freetype','mingw64-libpng','mingw64-libxml2','mingw64-libusb1','mingw64-bzip2','mingw64-zlib','mingw64-win-iconv','mingw64-winpthreads','mingw64-libstdc++','google-noto-sans-mono-vf-fonts']
versions=subprocess.check_output(['rpm','-q',*packages],text=True)
from datetime import datetime,timezone
info='Windows x64 test build UTC: '+datetime.now(timezone.utc).isoformat()+'\n'
info+='Built with scripts/build-windows.sh on Fedora 43.\n'
info+='SDL 2.32.10; SoapySDR 0.8.1; SoapyHackRF 0.3.4; HackRF 2024.02.1; libiio 0.26.\n'
info+='FFTW 3.3.10 single precision with SSE2 (x64 baseline), built from source.\n'
info+='Upstream source URLs and checksums are recorded in the project repository: scripts/build-windows.sh and scripts/windows/deps.sha256\n'
info+='FreeType is used under the FreeType License; see licenses/mingw64-freetype/FTL.TXT.\n\n'+versions
(build/'BUILD-INFO.txt').write_text(info)
# Debug symbols are useful locally, but are not needed by the receiver.
for binary in [out/'atv-rx.exe',*out.rglob('*.dll')]:
 subprocess.run(['x86_64-w64-mingw32-strip','--strip-debug',str(binary)],check=True)
archive=shutil.make_archive(str(build/'atv-receiver-windows-x64'),'zip',build,out.name)
import hashlib
checksum=hashlib.sha256(Path(archive).read_bytes()).hexdigest()
Path(archive+'.sha256').write_text(checksum+'  '+Path(archive).name+'\n')
print(f'ZIP: {archive} ({Path(archive).stat().st_size/1048576:.1f} MiB)')
