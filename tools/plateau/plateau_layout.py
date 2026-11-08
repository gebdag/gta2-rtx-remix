"""The Plateau-Mont-Royal street table that make_plateau_map.py draws.

Coordinates are metres on Montreal's street grid, not true compass bearings:
x runs "Montreal east" from the centre line of avenue du Parc, y runs "Montreal
north" from the centre line of rue Sherbrooke. Montreal maps and street signs use
this rotated grid (about 55 degrees off true north), and so does this table.

The positions are estimates written by hand, not survey data. They keep the
street order, the main one-way directions and the size of the parks close to
the real neighbourhood. Edit a number here and run make_plateau_map.py again to
move a street.

Street fields:
    name   - shown in the preview and the generator's log
    axis   - 'ns' (runs Montreal north-south) or 'ew'
    pos    - metres: x for an 'ns' street, y for an 'ew' street
    lanes  - one 1-block lane per lane of traffic
    dir    - 'two' for two-way; 'n', 's', 'e' or 'w' for one-way
    kind   - 'boulevard', 'commercial', 'residential' or 'ring'
    spans  - list of (from, to) cross-street names; the street runs between
             them. None means the whole map, ring road to ring road.
    median - True puts a strip of grass between the two directions
"""

# Edges of the playable area, in the metres above. The ring road sits on them.
WEST, EAST = -130.0, 2130.0
SOUTH, NORTH = -95.0, 1675.0

RING = [
    dict(name='Ring road (west)', axis='ns', pos=WEST, lanes=2, dir='two', kind='ring', spans=None),
    dict(name='Ring road (east)', axis='ns', pos=EAST, lanes=2, dir='two', kind='ring', spans=None),
    dict(name='Ring road (south)', axis='ew', pos=SOUTH, lanes=2, dir='two', kind='ring', spans=None),
    dict(name='Ring road (north)', axis='ew', pos=NORTH, lanes=2, dir='two', kind='ring', spans=None),
]

NORTH_SOUTH = [
    dict(name='Avenue du Parc', pos=0, lanes=4, dir='two', kind='boulevard'),
    dict(name='Rue Jeanne-Mance', pos=85, lanes=1, dir='n', kind='residential',
         spans=[('Ring road (south)', 'Rue Duluth')]),
    dict(name="Avenue de l'Esplanade", pos=165, lanes=1, dir='s', kind='residential'),
    dict(name='Rue Saint-Urbain', pos=255, lanes=2, dir='s', kind='residential'),
    dict(name='Rue Clark', pos=335, lanes=1, dir='n', kind='residential'),
    dict(name='Boulevard Saint-Laurent', pos=415, lanes=2, dir='n', kind='commercial'),
    dict(name='Rue Saint-Dominique', pos=485, lanes=1, dir='s', kind='residential'),
    dict(name='Rue de Bullion', pos=560, lanes=1, dir='n', kind='residential'),
    dict(name="Avenue de l'Hotel-de-Ville", pos=635, lanes=1, dir='s', kind='residential'),
    dict(name='Avenue Laval', pos=705, lanes=1, dir='n', kind='residential',
         spans=[('Rue Sherbrooke', 'Rue Duluth')]),
    dict(name='Rue Henri-Julien', pos=775, lanes=1, dir='s', kind='residential',
         spans=[('Rue Roy', 'Ring road (north)')]),
    dict(name='Rue Saint-Denis', pos=870, lanes=2, dir='two', kind='commercial'),
    dict(name='Rue Rivard', pos=950, lanes=1, dir='n', kind='residential',
         spans=[('Rue Roy', 'Ring road (north)')]),
    dict(name='Rue Berri', pos=1030, lanes=1, dir='s', kind='residential'),
    dict(name='Rue Saint-Hubert', pos=1110, lanes=2, dir='two', kind='residential'),
    dict(name='Rue Saint-Andre', pos=1195, lanes=1, dir='n', kind='residential'),
    dict(name='Avenue Christophe-Colomb', pos=1285, lanes=2, dir='two', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Avenue du Parc-La Fontaine', pos=1380, lanes=2, dir='two', kind='residential',
         spans=[('Ring road (south)', 'Rue Rachel')]),
    dict(name='Rue Boyer', pos=1380, lanes=1, dir='s', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Rue de Mentana', pos=1465, lanes=1, dir='n', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Rue de Brebeuf', pos=1555, lanes=1, dir='s', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Rue Garnier', pos=1645, lanes=1, dir='n', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Rue Fabre', pos=1735, lanes=1, dir='s', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Rue de Lanaudiere', pos=1825, lanes=1, dir='n', kind='residential',
         spans=[('Rue Rachel', 'Ring road (north)')]),
    dict(name='Avenue Papineau', pos=1930, lanes=4, dir='two', kind='boulevard'),
    dict(name='Rue Cartier', pos=2040, lanes=1, dir='s', kind='residential'),
]

EAST_WEST = [
    dict(name='Rue Sherbrooke', pos=0, lanes=4, dir='two', kind='boulevard'),
    dict(name='Rue Cherrier', pos=110, lanes=1, dir='w', kind='residential',
         spans=[('Rue Saint-Denis', 'Avenue du Parc-La Fontaine')]),
    # Pedestrian between Saint-Laurent and the Carre Saint-Louis: see PLAZAS.
    dict(name='Rue Prince-Arthur', pos=120, lanes=1, dir='e', kind='residential',
         spans=[('Avenue du Parc', 'Boulevard Saint-Laurent')]),
    dict(name='Rue Roy', pos=290, lanes=1, dir='e', kind='residential',
         spans=[('Avenue du Parc', 'Avenue du Parc-La Fontaine'),
                ('Avenue Papineau', 'Ring road (east)')]),
    dict(name='Rue Duluth', pos=420, lanes=1, dir='w', kind='commercial',
         spans=[('Avenue du Parc', 'Avenue du Parc-La Fontaine'),
                ('Avenue Papineau', 'Ring road (east)')]),
    # Rachel ends at avenue du Parc, at the foot of the mountain.
    dict(name='Rue Rachel', pos=590, lanes=2, dir='two', kind='commercial',
         spans=[('Avenue du Parc', 'Ring road (east)')]),
    dict(name='Rue Marie-Anne', pos=770, lanes=1, dir='e', kind='residential',
         spans=[("Avenue de l'Esplanade", 'Ring road (east)')]),
    dict(name='Avenue du Mont-Royal', pos=940, lanes=2, dir='two', kind='commercial'),
    dict(name='Rue Gilford', pos=1110, lanes=1, dir='w', kind='residential',
         spans=[('Avenue du Parc', 'Ring road (east)')]),
    dict(name='Avenue Laurier', pos=1290, lanes=2, dir='two', kind='commercial'),
    dict(name='Boulevard Saint-Joseph', pos=1500, lanes=4, dir='two', kind='boulevard', median=True),
]

# Rectangles in metres: (west, south, east, north). Parks are grass with paths;
# water is a pond one level below the street.
PARKS = [
    # Parc La Fontaine: Sherbrooke to Rachel, avenue du Parc-La Fontaine to Papineau.
    dict(name='Parc La Fontaine', rect=(1380, 0, 1930, 590),
         paths=[('ew', 300), ('ns', 1655)]),
    dict(name='Parc Jeanne-Mance', rect=(0, 420, 165, 940), paths=[('ew', 680)]),
    # The foot of Mount Royal, west of avenue du Parc.
    dict(name='Mont Royal', rect=(WEST, 100, 0, 1100), paths=[('ew', 590)]),
    dict(name='Carre Saint-Louis', rect=(705, 70, 870, 170), paths=[('ew', 120), ('ns', 787)]),
]

WATER = [
    # The two linked ponds in the south half of Parc La Fontaine.
    dict(name='Etang du parc La Fontaine (west)', rect=(1480, 120, 1620, 260)),
    dict(name='Etang du parc La Fontaine (east)', rect=(1600, 170, 1800, 270)),
    dict(name='Fontaine du Carre Saint-Louis', rect=(770, 100, 805, 140)),
]

# Pedestrian streets: paved, no traffic arrows. (west, south, east, north).
PLAZAS = [
    dict(name='Rue Prince-Arthur (pietonne)', rect=(415, 105, 705, 135)),
    # The plaza at the Sir George-Etienne Cartier monument, where the tam-tams play.
    dict(name='Monument George-Etienne Cartier', rect=(-75, 540, 0, 640)),
]

# Buildings that stand out. levels counts storeys above the street.
LANDMARKS = [
    dict(name='Eglise Saint-Jean-Baptiste', rect=(715, 615, 800, 700), levels=5,
         wall='wall_stone', tower=(715, 615, 740, 640), tower_levels=6),
    dict(name='Monument George-Etienne Cartier', rect=(-45, 575, -25, 605), levels=4,
         wall='wall_stone'),
    dict(name='Chalet du parc La Fontaine', rect=(1630, 300, 1700, 345), levels=2,
         wall='wall_stone'),
    dict(name='Metro Mont-Royal', rect=(975, 950, 1010, 985), levels=2, wall='wall_commercial'),
    dict(name='Metro Sherbrooke', rect=(1045, 105, 1085, 140), levels=2, wall='wall_commercial'),
]

# Named areas for the game's navigation zones: (name, west, south, east, north).
# GTA2 shows a zone's name from its text file (.gxt) when you drive in; a name
# that file does not know may show blank. The generator writes these only when
# asked (--nav-zones).
ZONES = [
    ('plateau', WEST, SOUTH, EAST, NORTH),
    ('mntroyal', WEST, SOUTH, 0, NORTH),
    ('stlaur', 335, SOUTH, 500, NORTH),
    ('stdenis', 800, SOUTH, 950, NORTH),
    ('parcfont', 1380, 0, 1930, 590),
]


def streets():
    """Every street with its axis filled in and its spans resolved later."""
    out = [dict(s) for s in RING]
    for s in NORTH_SOUTH:
        out.append(dict(s, axis='ns'))
    for s in EAST_WEST:
        out.append(dict(s, axis='ew'))
    for s in out:
        s.setdefault('spans', None)
        s.setdefault('median', False)
    return out
