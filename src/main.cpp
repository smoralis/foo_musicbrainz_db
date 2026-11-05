#include <foobar2000.h>
#include <component.h>

DECLARE_COMPONENT_VERSION(
    "MusicBrainz Database Provider",
    "0.1.0",
    "MusicBrainz metadata cache and JSON repository provider");

VALIDATE_COMPONENT_FILENAME("foo_musicbrainz_db.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
