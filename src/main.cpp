#include <foobar2000.h>
#include <component.h>

DECLARE_COMPONENT_VERSION(
    "MusicBrainz Database Provider",
    "0.1.10",
    "MusicBrainz metadata cache and JSON repository provider.\n"
    "SDK integration prototype for foobar2000 2.x.");

VALIDATE_COMPONENT_FILENAME("foo_musicbrainz_db.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
