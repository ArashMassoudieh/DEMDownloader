# demcheck — Highest-resolution USGS 3DEP DEM lookup + downloader

Reads a CSV of coordinates and, for each point, queries the USGS
**TNMAccess API** (`tnmaccess.nationalmap.gov/api/v1/products`) from finest
to coarsest 3DEP resolution, reporting the best available DEM. Optionally
downloads the matching tiles into a per-project folder.

Resolution tiers checked, in order:
1 meter -> 1/9 arc-second -> 1/3 arc-second -> 1 arc-second.

## Build

Qt5 or Qt6 (Core + Network). Two options:

**qmake**
    qmake demcheck.pro
    make

**cmake**
    cmake -B build
    cmake --build build

## Usage

    ./demcheck input.csv [output.csv] [options]

Options:
  --lat-col NAME    Latitude column (default: auto-detect lat/latitude/...)
  --lon-col NAME    Longitude column (default: auto-detect lon/lng/longitude/...)
  --id-col NAME     Identifier column; also names the download subfolder.
  --buffer METERS   Half-width of the query box around each point (default 30).
  --download DIR    Download matching tiles into DIR/<id>/ (one folder/project).
  --max-tiles N     Cap tiles downloaded per site (default 8; 0 = no limit).

Input needs a header row; coordinates are decimal degrees (WGS84).
Auto-detected column names include those exported from the WS3 register
("Centroid Lat", "Centroid Lon", "Site Lat", "Site Lon").

## Examples

Check only (no download):
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No."

Check and download into ./DEMs/<Project No.>/ :
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No." --download ./DEMs

Wider coverage check around community centroids, more tiles:
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No." \
        --download ./DEMs --buffer 3000 --max-tiles 0

## Output

Input columns, plus:
  best_resolution, best_dataset, tile_count, best_dem_date,
  best_download_url, query_status
And when --download is used:
  tiles_downloaded, download_dir

`query_status` values:
  ok                              best_resolution is valid
  no coverage                     all tiers answered, none had data
  incomplete: some tiers errored  a finer DEM may exist but a query failed
  API error: no tiers reachable   network/endpoint problem
  missing/invalid coordinate      row had no usable lat/lon

## Download behavior

- One subfolder per site, named from --id-col (e.g. DEMs/AZ12-301/).
- Streams each tile to <name>.part, then renames on success, so an
  interrupted download is never mistaken for a complete file.
- Skips files that already exist at the expected size -> re-runs are cheap
  and resume where they left off.
- Follows HTTP redirects (USGS download links redirect to a CDN).

## Cautions

- 1 m DEM tiles are large (often 100-400 MB each). A wide --buffer over a
  1 m coverage area can match many tiles; --max-tiles caps the count.
- Community-centroid coordinates may sit some distance from the actual site;
  a tile found at the centroid is not proof it covers the subdivision.
  Use a larger --buffer to confirm footprint coverage.

## Note on dataset names

If USGS renames a 3DEP dataset, the only thing to edit is the `RESOLUTIONS`
table at the top of `main.cpp`. Run one point first and check the JSON if a
tier unexpectedly returns nothing.
