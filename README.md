## Tab5DOS

> Tab5DOS는 M5Stack Tab5의 ESP32-P4에서 DOS 디스크 이미지를 부팅하는 PC 호환 에뮬레이터입니다. microSD 카드의 FreeDOS 또는 MS-DOS 이미지를 읽고, PC 텍스트 및 그래픽 화면을 LCD에 표시하며, USB 키보드와 터치 입력을 DOS 환경으로 전달합니다.

이 프로젝트는 다음 사용 흐름을 목표로 합니다.

1. microSD 카드에 부팅 가능한 DOS 이미지를 준비합니다.
2. Tab5를 부팅하면 우선순위가 가장 높은 이미지를 자동으로 선택합니다.
3. USB 키보드 또는 터치 입력으로 DOS 프로그램을 실행합니다.
4. 변경된 디스크 섹터는 원본 이미지에 다시 기록됩니다.

> 디스크 쓰기 기능은 원본 이미지 파일을 직접 변경합니다. 중요한 이미지는 실행 전에 반드시 별도로 백업하십시오.

## 주요 기능

- 8086 호환 실모드 CPU와 일부 80186 명령 실행
- FreeDOS 및 MS-DOS 플로피, 하드 디스크 이미지 부팅
- BIOS 디스크 읽기와 쓰기 및 지연식 `fsync` 쓰기 반영
- 80열 텍스트, CGA, Hercules, VGA 평면 및 256색 화면 출력
- VGA 사용자 글꼴과 텍스트 커서, 팔레트, 화면 이동 상태 반영
- USB Host 키보드
- LIM EMS 4.0 부분 구현
  - 4 MiB 확장 메모리 풀
  - E000:0000의 64 KiB 페이지 프레임
  - 16 KiB 단위 페이지 매핑
- 화면 변경 감지와 PPA 기반 회전 및 확대
- 직렬 모니터를 통한 RGB565 화면 캡처

## 하드웨어

- M5Stack Tab5
- microSD 카드
- USB Host 포트에 연결할 USB 키보드

## 개발 환경

ESP-IDF 환경 설정과 장치 연결은 사용하는 운영체제의 ESP-IDF 설치 지침에 따라 완료해야 합니다.

### microSD 카드 준비

microSD 카드에 다음 디렉터리를 만듭니다.

```text
/sdcard/dos/
```

부팅 가능한 `.img` 또는 `.ima` 파일을 이 디렉터리에 복사합니다. 실제 카드에서는 다음과 같은 구조가 됩니다.

```text
/dos/
  fd0.img
  hd0.img
```

펌웨어 내부의 마운트 경로는 `/sdcard/dos/`입니다.

### 이미지 선택 우선순위

다음 이름은 대소문자를 구분하지 않고 우선 선택됩니다. 파일 이름이 `hd`로 시작하거나 크기가 2,880 KiB보다 크면 BIOS 하드 디스크 `80h`로 연결됩니다. 그 외 이미지는 플로피 디스크 `00h`로 연결됩니다.

1. `fd0.img`
2. `a.img`
3. `hd20_DOSPROG.img`
4. `floppy_freedos.img`
5. `freedos.img`
6. `hd0.img`
7. 나머지 `.img` 및 `.ima` 파일의 이름순

## 빌드와 실행

환경에 `idf-venv` 보조 명령이 구성되어 있다면 다음처럼 사용할 수 있습니다.

```sh
$ idf-venv
(idf-venv) $ idf.py set-target esp32p4
(idf-venv) $ idf.py build flash monitor
```

### 선택 빌드 기능

`Tab5DOS_AUTOTEST_KEYS`를 활성화하면 부팅 후 DOS 명령을 자동으로 입력하여 프롬프트, 디렉터리 조회, 파일 쓰기와 재조회 흐름을 점검합니다. 이 기능은 일회성 하드웨어 점검용이며 일반 사용 시에는 비활성화하는 것이 좋습니다.

```sh
$ Tab5DOS_AUTOTEST_KEYS=1 idf.py reconfigure build flash monitor
```

### CPU 프로파일링

`Tab5DOS_CPU_PROFILE`을 활성화하면 opcode와 CS:IP별 샘플 실행 시간을 직렬 로그로 출력합니다. 프로파일링은 실행 오버헤드를 추가하므로 성능 기준 측정이나 일반 사용에는 기본 빌드를 사용하십시오.

```sh
$ Tab5DOS_CPU_PROFILE=1 idf.py reconfigure build flash monitor
```

## 화면 캡처

Tab5 화면의 오른쪽 위 모서리를 약 1.5초 동안 길게 누르면 현재 PC 원본 프레임이 직렬 모니터로 출력됩니다. 화면 복사 후 인코딩과 출력은 별도 저우선순위 태스크에서 처리됩니다.

모니터 출력을 파일로 저장합니다.

```sh
idf.py monitor | tee screenshot.log
```

로그에 여러 캡처가 있으면 `screenshot_000.bmp`, `screenshot_001.bmp`처럼 번호가 붙은 파일이 생성됩니다. 특정 캡처만 변환하려면 0부터 시작하는 인덱스를 지정합니다.

```sh
uv run --python 3.14 tools/decode_screenshot.py screenshot.log selected.bmp --index 1
```

화면 캡처 데이터는 `rgb565le` 또는 `rgb565le-rle` 형식으로 기록됩니다.

## 디스크 쓰기와 데이터 보존

게스트의 BIOS 디스크 쓰기는 열린 이미지 파일에 반영됩니다. 변경된 섹터는 메모리에 잠시 보관한 뒤 약 1초 간격으로 `fsync`됩니다.

실기 점검 절차는 다음과 같습니다.

1. DOS 프롬프트가 표시될 때까지 기다립니다.
2. `dir`을 실행합니다.
3. 작은 파일을 생성합니다.
4. 디스크 쓰기 로그가 출력되는지 확인합니다.
5. 안전하게 재부팅한 뒤 생성한 파일이 남아 있는지 확인합니다.

전원 차단 직전의 미반영 쓰기를 피하려면 디스크 작업이 끝난 뒤 최소 1초 이상 기다리십시오.

## 직렬 로그

주요 실행 상태는 직렬 모니터와 `/sdcard/dos/Tab5DOS.log`에 기록됩니다.

```text
before PcMachine init heap: internal/8bit=<bytes> psram=<bytes>
after PcMachine init heap: internal/8bit=<bytes> psram=<bytes>
EMS: LIM 4.0, 4096 KiB, page frame e000
Boot image: /sdcard/dos/<image>.img
USB boot keyboard connected
first USB keyboard input report received
TEXT80 milestone: DOS prompt detected
TEXT80 milestone: DIR listing detected
DISK milestone: write count=<n> drive=00 lba=<sector> sectors=<count>
TEXT80 milestone: DOS-created file detected
```

미지원 opcode나 포트, 인터럽트가 감지되면 진단 카운터와 마지막 발생 위치가 로그에 출력됩니다.

## 호스트 테스트

ESP 보드 없이 PC 코어의 회귀 테스트를 실행할 수 있습니다.

```sh
cmake -S tests -B .test-build -DCMAKE_BUILD_TYPE=Release
cmake --build .test-build -j4
ctest --test-dir .test-build --output-on-failure
```

현재 테스트는 다음 영역을 포함합니다.

- BIOS 및 머신 통합 동작
- CGA, Hercules, VGA 및 텍스트 렌더링
- 디스크 카탈로그와 디스크 이미지 읽기 및 쓰기
- 키보드 컨트롤러
- 8086 명령
- EMS 명령

AddressSanitizer와 UndefinedBehaviorSanitizer를 이용하려면 별도 빌드 디렉터리를 사용합니다.

```sh
cmake -S tests -B .test-build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build .test-build-sanitize -j4
ctest --test-dir .test-build-sanitize --output-on-failure
```

## 프로젝트 구조

| 경로 | 설명 |
| --- | --- |
| `components/Tab5DOS_pc/pc/` | CPU, BIOS, 디스크, 키보드, 비디오, EMS, OPL2 에뮬레이션 코어 |
| `main/main.cpp` | 부팅, 태스크 구성, 디스크 선택, 입력과 실행 흐름 |
| `main/tab5_lcd_text.*` | LCD 출력, PPA 변환, 화면 캡처 |
| `main/tab5_usb_keyboard.*` | USB Host 키보드 연결과 HID 보고서 처리 |
| `main/tab5_speaker.*` | PC 스피커와 OPL2 오디오 출력 |
| `tests/` | 호스트 CMake 회귀 테스트 |
| `tools/decode_screenshot.py` | 직렬 화면 캡처를 BMP로 변환하는 도구 |

## 현재 제한 사항

- 완전한 IBM PC, VGA, OPL2 하드웨어 구현은 아닙니다.
- 일부 DOS 프로그램은 미지원 opcode, BIOS 기능 또는 주변 장치 때문에 실행되지 않을 수 있습니다.
- EMS는 LIM EMS 4.0의 호환성에 필요한 일부 기능을 구현합니다.
- 게임 실행 속도와 OPL2 음악 동기화는 프로그램에 따라 부족할 수 있습니다.
- ESP32-P4 실기 성능은 디스크 이미지, 화면 모드와 프로그램의 CPU 사용 패턴에 따라 달라집니다.
