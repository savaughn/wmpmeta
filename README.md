# wmpmeta

Brings back CD album and track names in Windows Media Player 9 on Windows 2000, using MusicBrainz.

Microsoft shut down the metadata service WMP uses, so every audio CD now shows up as "Unknown Artist" with tracks named "Track 1", "Track 2" and so on. wmpmeta is a small program that runs on the same PC and answers WMP's lookups itself. It is a single 700 KB exe with its own TLS 1.2 stack and root certificates, so it doesn't need a runtime or a modern machine on the network.

## Install

Download `wmpmeta-<version>-setup.exe` from Releases and run it as an Administrator. Then close Windows Media Player, open it again, and insert a CD.

Setup installs to `Program Files\wmpmeta` and does three things:

- adds `windowsmedia.com`, `www.windowsmedia.com`, `services.windowsmedia.com` and `go.microsoft.com` to the hosts file, pointing at 127.0.0.1, after backing it up as `hosts.bak-wmpmeta`
- starts `wmpmeta.exe` at logon from `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`
- starts wmpmeta

You must reopen WMP after installing. A copy of WMP that is already running keeps its connections to Microsoft's old server open.

To remove it, use Add/Remove Programs. The uninstaller stops wmpmeta and removes the hosts lines and the logon entry.

For a silent install, run `setup.exe /S`. If you'd rather not run an installer, `wmpmeta-<version>.zip` has the same exe plus `install.cmd` and `uninstall.cmd`, which do the same setup from whatever folder you unzip to.

## Requirements

- Windows 2000 with Windows Media Player 9. Other combinations, such as XP with WMP 9 or 10, haven't been tested.
- Internet access to `musicbrainz.org`, `coverartarchive.org`, `archive.org` and `download.microsoft.com`.
- A correct system clock. Certificate checks fail if the date is far off; when that happens the log says so.
- Nothing else listening on 127.0.0.1 port 80.

## How it works

WMP9 sends `GET http://windowsmedia.com/redir/GetMDRCD.asp?CD=<TOC>`. The TOC is the track count, each track's start offset in CD frames (including the 150-frame pregap) and the lead-out, all in hex, for example `E+96+4F38+…+418B8`. wmpmeta computes the MusicBrainz disc ID from the TOC and looks it up. It then answers in the `MDR-CD` XML format WMP expects: album, artist, release date, label, genre, and each track's title and artist.

Lookup order:

1. Exact disc ID.
2. The disc ID with the last track treated as an Enhanced CD data track. MusicBrainz leaves data tracks out of disc IDs.
3. MusicBrainz's fuzzy TOC search, which helps with pressings that aren't in the database. A fuzzy match only counts if every track length is within 3 seconds, which keeps it from naming the wrong album.

Cover art comes from the Cover Art Archive through `services.windowsmedia.com/cover/<release ID>`. Requests to MusicBrainz are spaced at least 1.1 seconds apart, as its API rules require, and results are cached until wmpmeta exits.

Every request WMP makes, including ones wmpmeta doesn't handle, is logged to `wmpmeta.log` next to the exe.

## Album info in Copy from CD

- **View Album Info** shows the album wmpmeta matched: cover, release date, label, format and the track list with lengths.
- **Find Album Info** lists the MusicBrainz releases that fit the CD, or searches MusicBrainz by artist and album name. **Use this** picks a release; WMP renames the tracks right away, and wmpmeta remembers the choice for that CD in `wmpmeta-albums.txt`.

## More Skins, Visualizations and Plug-ins

The **More Skins** button in Skin Chooser goes to `go.microsoft.com/fwlink/?LinkId=9705`. wmpmeta answers it with a gallery of the 75 skins Microsoft's Windows Media Player 9 Series skin gallery offered from 2002 to 2005. The list and preview pictures come from the Internet Archive's copies of that gallery. Click a skin, choose **Open**, and WMP installs it.

The skins themselves aren't inside wmpmeta, because they belong to Microsoft and the studios that made them. The first time you pick one, wmpmeta downloads it from `download.microsoft.com` (most are still there) or the Internet Archive and keeps it in the `skins` folder. To download all of them at once (about 85 MB), run `wmpmeta -getskins`.

**More Visualizations** (`LinkId=9706`) works the same way. It opens a gallery of the 14 visualization packs from the WMP9 visualization gallery, such as Averett's Trilogy, Picture Viz and Dungeon Siege. Each one is a setup program: download it, run it, and the new visualizations appear in Now Playing. A few were made for Windows XP and may not install on Windows 2000. `wmpmeta -getskins` downloads these too, into `skins\visualizations`.

**More Plug-ins** (`LinkId=7262`) shows the WMP9 plug-in gallery. Microsoft's free PowerToys (the "Decades" Auto Playlist Pack, Ratings Migration and TweakMP) download from the Internet Archive. The third-party plug-ins, such as DFX, SRS and the DVD and MP3 packs, were sold from their makers' sites, which are gone, so they're listed for reference only.

Other `go.microsoft.com` links are passed on to Microsoft's server, so they go where they always did.

## The other WMP9 tabs

- **Media Guide** shows music news and new album reviews from public RSS feeds, styled after the 2002 WindowsMedia.com.
- **Radio Tuner** lists MP3 stations from radio-browser.info by genre, search or country. Streams play through wmpmeta, which handles HTTPS stations and redirects for WMP.
- **Premium Services** shows Microsoft's own page from July 26, 2003, fetched from the Internet Archive the first time the tab opens and kept in `wayback-services.html`. The archive didn't keep its logos, so the service names stand in for them.

## Settings

Optional. Put a `wmpmeta.ini` next to `wmpmeta.exe` and restart wmpmeta (or log off and on).

```
; Media Guide news column (repeat for more feeds; replaces the defaults)
news=https://pitchfork.com/feed/feed-news/rss
news=https://www.stereogum.com/feed/
; Media Guide album list
albums=https://pitchfork.com/feed/feed-album-reviews/rss
; show ListenBrainz's weekly artist chart (off by default)
charts=1
; leave stations from these countries out of Radio Tuner (ISO codes)
radio_hide_countries=IN
```

## Command line

```
wmpmeta            run in the background (no window)
wmpmeta -stop      stop the running copy
wmpmeta -v         also log the XML sent to WMP
wmpmeta -p PORT    listen on another port (for testing)
wmpmeta -q TOC     look up one TOC, write the result to the log, and exit
wmpmeta -getskins  download everything the galleries offer into the skins folder
```

## Building

Cross-compile from macOS or Linux with MinGW-w64:

```
make            # build/wmpmeta.exe
make dist       # dist/wmpmeta-<version>-setup.exe and dist/wmpmeta-<version>.zip
make check W2K_SYSDIR=/path/to/win2k/system32
```

The setup.exe needs NSIS 3. The official Windows `makensis.exe` works under Wine:

```
make dist MAKENSIS="wine64 /path/to/nsis-3.10/makensis.exe" NSIS_DRIVE=Z:
```

`make check` confirms that every function the exe imports exists in the real Windows 2000 SP4 system DLLs, and that the PE header says OS version 5.0. Newer MinGW runtimes pull in XP-only functions unless they're replaced, and `src/compat.c` provides those.

The TLS code in `src/tls.c`, `src/util.c` and `src/compat.c` comes from tlsproxy, a TLS 1.2 proxy for Windows 2000. Mbed TLS uses CryptGenRandom for entropy and `gethostbyname` for name lookup, because Windows 2000 has no `getaddrinfo`.

## License

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY_NOTICES.txt`. Album data comes from [MusicBrainz](https://musicbrainz.org) (CC0). Not affiliated with Microsoft.
