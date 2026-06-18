# demcheck — USGS highest-resolution DEM + hydrography lookup/downloader

Reads a CSV of coordinates and, for each point, queries the USGS **TNMAccess
API** for the highest-resolution data available, then optionally downloads it
into a per-site, per-product folder.

Two product types:
  - **elevation** (3DEP DEM): 1 m -> 1/9 -> 1/3 -> 1 arc-second, GeoTIFF tiles.
  - **hydrography** (flowlines): NHDPlus HR -> NHD Best Resolution, as zipped
    Shapefile packages (delivered by watershed unit, so usually one per point).

Note: USGS retired the NHD on 1 Oct 2023 (still downloadable, no longer
maintained); NHDPlus HR is the current highest-resolution flowline product and
is tried first. The 3D Hydrography Program (3DHP) is the long-term successor.

## Build

Qt5 or Qt6 (Core + Network).

    qmake demcheck.pro && make
    # or
    cmake -B build && cmake --build build

## Usage

    ./demcheck input.csv [output.csv] [options]

Options:
  --products LIST   dem, hydro, or all (comma-separated). Default: dem
  --lat-col NAME    Latitude column (default: auto-detect lat/latitude/...)
  --lon-col NAME    Longitude column (default: auto-detect lon/lng/...)
  --id-col NAME     Identifier column; also names download subfolders.
  --buffer METERS   Half-width of the query box around each point (default 30).
  --download DIR    Download data into DIR/<id>/<product>/.
  --max-tiles N     Cap files per product per site (0 = no limit;
                    default is per-product: 8 for DEM, 4 for hydro).
  --make-points     Write a single-point shapefile per site (.shp/.shx/.dbf/
                    .prj/.cpg) at the site's coordinate, with all other CSV
                    columns carried in as attributes.
  --points-dir DIR  Base dir for point shapefiles (default: the --download dir
                    if set, else the output CSV's folder).

Input needs a header row; coordinates are decimal degrees (WGS84).

## Examples

Check DEM only:
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No."

Check both DEM and hydrography:
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No." --products all

Check and download both into per-site folders:
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No." \
        --products all --download ./GIS --buffer 500

Just create point shapefiles (no data download):
    ./demcheck WS3_Site_Coordinates.csv --id-col "Project No." \
        --make-points --points-dir ./points

## Output

Input columns, plus a block per selected product, prefixed by product key
(dem_ / hydro_):
  <p>_best_resolution, <p>_best_dataset, <p>_count, <p>_date,
  <p>_url, <p>_status   (+ <p>_downloaded, <p>_dir when --download is used)

`*_status` values: ok | no coverage | incomplete: some tiers errored |
API error: no tiers reachable | missing/invalid coordinate

## Download layout

    DIR/
      AZ12-100/
        dem/      <GeoTIFF tiles>
        hydro/    <zipped Shapefile package>
      AZ12-301/
        dem/
        hydro/
        point/    <single-point AZ12-301.shp + sidecars>

Files stream to <name>.part then rename on success; existing files are skipped
on re-run (compared by size), so interrupted runs resume cheaply.

## Code structure (object-oriented)

main.cpp only parses arguments and wires objects. Logic lives in src/:
  Types.h          plain data structs (Tier, Tile, QueryResult, ProductOutcome)
  ProductType.h    abstract product category + ElevationProduct /
                   HydrographyProduct subclasses (tiers, formats, defaults)
  TnmClient.*      all HTTP: querying a dataset, downloading a file
  CsvTable.*       CSV parse / column detection / write
  SiteProcessor.*  orchestration across sites and products
  ShapefileWriter.* writes a minimal single-point .shp/.shx/.dbf/.prj/.cpg

Adding a new USGS product (e.g. WBD watershed boundaries) = one new ProductType
subclass and one line in main(); nothing else changes.

## Cautions

- 1 m DEM tiles are large (100-400 MB each); a wide --buffer can match many.
  --max-tiles caps the count per product.
- Hydrography packages are per-watershed and can be large; the same package
  often covers several nearby sites (you may download duplicates across sites).
- Community-centroid coordinates may sit some distance from the actual site;
  a hit at the centroid is not proof of coverage at the subdivision.

## Dataset name strings

If USGS renames a dataset, edit the tier tables in src/ProductType.h
(ElevationProduct::m_tiers, HydrographyProduct::m_tiers). Run one point and
check the JSON if a tier unexpectedly returns nothing.
