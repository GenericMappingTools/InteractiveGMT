# MODIS scenes (Terra / Aqua)

*Satellite → Remote sensing → MODIS scenes (Terra/Aqua)…*

Which MODIS scenes did Terra or Aqua take, when, and over where — for **any date**, past or present —
and the files NASA keeps for them. The tool does two things:

- **Plot scene footprints**: draw the ground area of every 5-minute MODIS scene in a time window.
- **Find scenes**: list the scenes that cover one point over a number of days.

Both give the scenes' files at NASA's Ocean Biology DAAC (OB.DAAC) as clickable download links.

The orbit is iGMT's own SGP4 propagator (the one behind *Satellite orbits…*); nothing is installed
and no external program is run. It is the last example of the RemoteS documentation
(*Aqua orbits*), made interactive.

## The dialog

The dialog has two tabs, **Scenes** and **Accounts**, and a status line under them. The status line
says what was done, how many scenes OB.DAAC does not have (see below) and how far the orbital
elements used are from the requested time.

### Satellite and orbital elements

- **Satellite** — AQUA or TERRA.
- **Elements** — where the orbit comes from. An orbit is computed from a *TLE* (two-line element
  set), and a TLE is only good for **a few days around its own date** (its *epoch*): used weeks away
  from it, the satellite's position can be off by tens of kilometres, which is a whole scene. So the
  right elements depend on the date you ask about:

| Elements | Use it for | Needs |
|---|---|---|
| **Celestrak (current elements)** | today and the last few days | nothing — downloaded automatically |
| **Space-Track history (the set nearest the date)** | **any past date** | a free Space-Track account (Accounts tab) |
| **TLE file (the set nearest the date)** | any date you have a file for | a file of element sets |

  With the last two, of all the sets available the one whose epoch is **nearest the requested time**
  is used. The status line always says which epoch that was, and warns when it is more than 15 days
  away.

- **TLE file** and its **…** button — only for the *TLE file* source: a text file of two- or
  three-line element sets. It may hold many satellites; only the chosen one (AQUA = NORAD 27424,
  TERRA = NORAD 25994) is read.

### Scene footprints

- **Start (UTC)** — the start of the time window. MODIS scenes are 5-minute granules starting on
  the 5-minute marks, so the start is rounded to the nearest 5 minutes. The dialog opens on *now*.
- **Minutes** — the length of the window; each 5 minutes is one scene.
- **Plot scene footprints** — draws one polygon per scene. On an **empty window** a map with
  coastlines is made around the scenes first; on a window that already shows something the
  footprints are laid over it, in its own frame. They land in Scene Objects as one line layer named
  after the satellite and the window, e.g. `AQUA scenes 2021-09-02 13:30+10m`.

### Find the scenes that cover a point

- **Lon**, **Lat** — the point.
- **Days** — how many days to search from **Start**: a negative number searches that many days
  *before* it, a positive one that many days *after* it.
- **When** — *Day passes*, *Night passes* or *Any time*. A pass that crosses the day/night line is
  kept as either.
- **Product** — *Chlorophyll-a (L2 OC)* or *Sea surface temperature (L2 SST)*. Chlorophyll is only
  measured in daylight, so a night pass has no OC file.
- **Find scenes** — a scene covers the point when the satellite's ground track passes within half a
  MODIS swath of it (1163 km).

### The scene files

The list under the two groups gives every scene as a link to its file,
`https://oceandata.sci.gsfc.nasa.gov/getfile/<file name>`. Clicking one opens it in your web
browser, which asks for your Earthdata login (see Accounts). Long lines scroll sideways; the list
opens scrolled to the end, where the file names are.

**How the names are found.** A scene's file name carries the time of its first scan:
`AQUA_MODIS.20210908T125500.L2.OC.nc`. The orbit gives the scene's minute exactly, but not its
seconds: NASA's files end in `00` or `01` in runs that no orbit predicts (`…T125500`, `…T134001`).
So every name is asked of OB.DAAC itself (its file search service), all scenes at once, and the name
it answers is the one shown. This also settles the version of the file:

- For recent days OB.DAAC only has the near-real-time file, `….L2.SST.NRT.nc`.
- Once reprocessed, a date also has the final file, `….L2.SST.nc`. When both exist, the final one
  is given.

A scene OB.DAAC does not have is **left out of the list**, and counted in the status line ("1 not
(yet) at OB.DAAC"): it is either not processed yet — the newest scenes appear with a delay of a few
hours — or it has no ocean data. When OB.DAAC cannot be reached at all, the computed names are
shown, marked *(unchecked)*; their seconds are then a guess. OB.DAAC's answers are kept for 10
minutes, so pressing the button again is immediate.

A progress window follows the three steps: reading the elements, propagating the orbit, and asking
OB.DAAC for the names, one step per scene answered, with the seconds waited counting up.

## Accounts

Two free accounts, each saved as one entry of the file **`.netrc`** in your home folder — the
standard place where curl, wget, GDAL and most download tools look for logins:

```
machine urs.earthdata.nasa.gov
    login <your Earthdata user name>
    password <your Earthdata password>
machine www.space-track.org
    login <your Space-Track e-mail>
    password <your Space-Track password>
```

The tab opens filled with what the file already holds. **Save login** writes or replaces that one
entry and keeps every other entry of the file as it is. The file is plain text: the passwords are
readable by anyone who can read your home folder, as for every program that uses `.netrc`. Setting
the environment variable `NETRC` to another path makes the tool use that file instead.

### NASA Earthdata

Downloading the scene files from OB.DAAC needs an Earthdata login. Register at
[urs.earthdata.nasa.gov](https://urs.earthdata.nasa.gov/users/new). Note that a link clicked in the
list is downloaded by your **web browser**, which asks for the login itself; the entry in `.netrc`
is what command-line tools (curl, wget, GDAL) use to fetch the same files.

### Space-Track

[Space-Track](https://www.space-track.org) is run by the US Space Force and keeps **every element
set ever published** for every satellite — several a day for Terra and Aqua, back to their launch.
Celestrak, by contrast, only serves the current ones. That archive is what the *Space-Track history*
source reads. Register at [space-track.org](https://www.space-track.org/auth/createAccount); the
account is free.

**The local history — each satellite's elements are kept.** Space-Track limits automated use and
asks that history be downloaded once, not again and again. So the elements are asked by calendar
month, and every answer is stored for good in `~/.gmt/iGMT/tle_history/`:

- `<NORAD>.tle` — all element sets downloaded so far for that satellite (`27424.tle` for Aqua,
  `25994.tle` for Terra), in date order;
- `<NORAD>.months` — the months already complete on disk.

For a date, the months covering it ±3 days are needed; only those not on disk yet are asked for, in
one Space-Track session. A **complete** month (one that ended more than a day ago) is never asked for
again, so working on dates you have used before needs no network and no login. The **current**
month is not complete — new sets are still being published — and is asked for again at most every
two hours. The files are plain TLE text: they can also be used as a *TLE file* anywhere else.

A refused login, or a Space-Track error, is reported in the status line. Space-Track may lock an
account after repeated failed logins: if the login is refused, check it on the website first.

## Planned

- **Download the files from inside iGMT**, with the Earthdata login in `.netrc`, and open them
  straight into *MODIS L2 swath to grid* — instead of going through the web browser.
- **A manual button** on the dialog opening this page, as the GMT tool dialogs have.

## Current limits

- The *Space-Track history* source is new: its query path has not yet been exercised against a
  real account.
- *MODIS L2 swath to grid* has not yet been tried on a real MODIS L2 file.
