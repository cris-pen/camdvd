# camdvd

[![CI](https://github.com/cris-pen/camdvd/actions/workflows/ci.yml/badge.svg)](https://github.com/cris-pen/camdvd/actions/workflows/ci.yml)

Recovers video from camcorder DVDs on macOS, including unfinalized DVD-R discs that the Mac reports as blank. It was built for a Hitachi DZ-MV550A whose discs could not be finalized without a long-discontinued AC adapter, and it works the same way for any camcorder that records MPEG-2 to DVD-R, DVD-RW or DVD-RAM. No finalizing, no modifying the camera, no Windows tools.

## Why the Mac says the disc is blank

Finalizing writes the file system and DVD menus. An unfinalized disc has neither, so macOS has nothing to mount. The video itself is already on the disc. `camdvd` skips the file system, asks the drive which areas hold recorded data, reads those sectors directly, and rebuilds the MPEG-2 stream into one file per scene.

## How it works

- **`bin/camdvd` (C, IOKit).** Opens the optical drive through IOKit's SCSI task interface with exclusive access and sends raw MMC commands: READ DISC INFORMATION and READ TRACK INFORMATION to find the recorded areas, then READ(10) to copy sectors into an image. A ddrescue-style map records the state of every sector, so rips resume after an interruption and damaged areas get three passes.
- **`extract.py` (Python and ffmpeg).** Scans the image as 2048-byte MPEG-2 program stream packs, reads the stream clock from each pack header and the scene, cell and time fields from the DVD navigation packs, and joins the bursts the camera wrote back into whole scenes. ffmpeg copies each scene losslessly and can also make ProRes 422 HQ or H.264 versions.
- **`camdvd-rescue` (bash).** One command per disc: builds the tool on first run, rips, rebuilds the scenes, opens the folder and ejects. Batch mode works through a stack of discs.
- **Simulated drive.** The drive layer has a second backend that simulates a drive from a config file (bad sectors, flaky sectors, a drive that drops off USB, missing track information, DVD-RAM), so the whole pipeline is tested without hardware.

## Status

Ripping and extraction have been used on real DZ-MV550A DVD-R discs. Scene grouping is tested against simulated burst-written discs, with and without scene numbers in the navigation packs. Reports from other camcorders and drives are welcome in the issues.

## Requirements

- macOS 11 or later, Apple Silicon or Intel
- Xcode command line tools and ffmpeg
- A tray-loading USB DVD drive. Slot-loading drives cannot take 8 cm discs.

## Setup (once)

1. Clone into your home folder:

   ```
   cd ~
   git clone https://github.com/cris-pen/camdvd.git
   ```
   Keep it out of Desktop, Documents, Downloads and iCloud Drive, because macOS blocks raw drive access for tools run from those folders.
2. Install the requirements:

   ```
   xcode-select --install     # skip if already installed (Homebrew needs it too)
   brew install ffmpeg
   ```
3. Connect the DVD drive.

The first run builds the drive tool automatically.

## Recover a disc

```
cd ~/camdvd
./camdvd-rescue birthday-2006
```

This creates `~/Movies/DVD Rescue/birthday-2006/` containing:

| File | What it is |
|---|---|
| `disc.img` | Raw copy of everything recorded on the disc. Your archival master. |
| `disc.img.map` | Which sectors were read. Lets an interrupted rip resume. |
| `disc-info.txt` | Disc type, tracks and recorded areas as the drive reported them. |
| `scenes/scene_001.mpg`, ... | One file per recording (scene), lossless copy of the original stream. |
| `scenes/scenes.txt` | Scene list with durations, why each scene starts where it does, and any damage notes. |

The scenes folder opens in Finder when it finishes and the disc ejects.

### How scenes are found

Camcorders write to the disc in short bursts with a few filler sectors between them, so the raw video on disc is in many short pieces. Pieces are joined back together unless the camera's own markers say a new scene began: the scene (VOB) number in the DVD navigation packs changes, or the stream clock restarts or jumps. Joins where the clock carries on are byte for byte, so nothing is re-encoded.

- `--join` also writes `whole_disc.mpg`, every scene back to back.
- `--split cells` also splits wherever the camera started a new DVD cell, if scenes come out merged that should not be.
- `--split pieces` writes every burst as its own file, which is useful for checking what is on the disc.

### Formats

```
./camdvd-rescue birthday-2006 --formats mpg,prores,mp4
```

- `mpg`: original MPEG-2 video and audio, untouched. Plays in VLC and IINA.
- `prores`: ProRes 422 HQ `.mov` with PCM audio, interlacing preserved. Drops straight into Premiere or Resolve.
- `mp4`: H.264/AAC, deinterlaced, for QuickTime, phones and sharing.

To add formats or rebuild the scenes later without touching the drive:

```
./camdvd-rescue birthday-2006 --extract-only --formats prores
```

### A stack of discs

```
./camdvd-rescue --batch xmas
```

Prompts for each disc in turn and names them `xmas-01`, `xmas-02` and so on. Discs already finished are skipped if you restart the batch.

## Damaged or difficult discs

The rip runs in three passes:

1. Fast pass. Reads 32 KB blocks and jumps over damaged areas instead of grinding on them.
2. Fill pass. Goes back for the areas that were jumped over.
3. Scrape pass. Retries each failed sector individually at a slower drive speed.

Anything still unreadable is zero-filled and listed. In the video that shows up as a brief glitch, and the footage on either side of it is unaffected. Exit status 2 means the rip finished with unreadable sectors.

- **Interrupted, or the drive drops off USB:** run the same command again. It resumes from the map and checks it is the same disc first.
- **Try the bad spots again:** clean the disc or connect a different drive, then:

  ```
  ./camdvd-rescue birthday-2006 --retry-bad
  ```
  The image and map do not depend on the drive, so a second drive fills in what the first one could not read.
- **Small unreadable runs right at the joins between bursts:** on DVD-R these are usually linking gaps the camera left between recordings, not lost footage.
- **"Nothing recorded was found" but the camera showed footage:** some drives do not report track information for unfinalized discs. Use `--probe` to read forward from the start of the disc until the recording ends:

  ```
  ./camdvd-rescue birthday-2006 --probe
  ```
- **Scenes split or joined in the wrong places:** rebuild from the saved image without the drive, for example `./camdvd-rescue birthday-2006 --extract-only --split cells`. `scenes.txt` lists the reason for every scene start, which shows which marker caused it.

## DVD-RAM discs

DVD-RAM does not need finalizing. It does need a drive that reads DVD-RAM, and many current drives have dropped that support. LG/Hitachi-LG drives with "DVDRAM" in the model name (for example the GP65NB60 or GP60NB50) still read it. 8 cm DVD-RAM discs ship in a cartridge; take the disc out of the cartridge before loading it in a tray drive.

## Errors

| Message | Fix |
|---|---|
| macOS refused to open the drive (0xe00002be) | The folder is in Desktop/Documents/Downloads/iCloud. Move it to `~/camdvd`. |
| The drive is busy | Quit DVD Player, Music, VLC or Disk Utility. If a "blank disc" dialog appeared, click Ignore. |
| No optical drives found | Reconnect the drive. Bus-powered drives may need both plugs of a Y-cable or a powered hub. |
| Disc format not readable by this drive | The drive does not support this disc type (common with DVD-RAM). Use a different drive. |

## Low-level tool

`bin/camdvd` can be used on its own:

```
bin/camdvd list                         # optical drives
bin/camdvd info                         # disc type, tracks, recorded areas
bin/camdvd rip -o disc.img              # read recorded sectors (resumable)
bin/camdvd rip -o disc.img --range 2048-700000
bin/camdvd eject

python3 extract.py disc.img -o scenes --format mpg,prores --join
```

## Tests

`tests/run_tests.sh` builds three test recordings, lays them out like an unfinalized camcorder DVD-R (reserved blank track, open recording track, management data between clips), and runs the full pipeline against a simulated drive with permanently bad sectors, flaky sectors that only read on retry, a drive that drops out mid-rip, a wrong-disc resume, a drive that reports no track information, and a DVD-RAM disc. `tests/make_burst_disc.py` adds discs written in bursts the way a camcorder writes them, with and without scene numbers in the navigation packs, to check that pieces rejoin into whole scenes.

Run them with:

```
make
tests/run_tests.sh
```

They need ffmpeg and run on macOS and Linux. GitHub Actions runs them on both, and builds the drive tool for Apple Silicon and Intel, on every push.

## License

MIT. See [LICENSE](LICENSE).
