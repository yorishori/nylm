# Shared by nylm's root actions and jobs; sourced, never run. Installed
# root-owned as /usr/local/lib/nylm/lib.sh (outside the actions folder, so
# sudo never runs it on its own).
#
# Reads /etc/nylm.conf (root's file) and checks its values by the same
# rules as nylm (src/sysinfo.c): what nylm accepts, root accepts, and
# nothing else.

set -f # no globbing: the settings are split into words below

NYLM_DATA= NYLM_MUSIC= NYLM_UNITS= NYLM_BACKUP= NYLM_BACKUP_DIR= NYLM_BACKUP_KEEP=
NYLM_BACKUP_GROUP=
. /etc/nylm.conf

# Every root job holds an exclusive flock on this folder while it runs.
JOBS_LOCK=/usr/local/lib/nylm/jobs

# nylm's own units: their logs are always shown.
NYLM_OWN_UNITS="nylm.service nylm-music-scan.service nylm-music-write.service
nylm-music-move.service nylm-qobuz.service nylm-disk-usage.service
nylm-updates-check.service nylm-update.service"

die() {
    echo "${0##*/}: $*" >&2
    exit 2
}

# A systemd unit name: 1..128 of A-Z a-z 0-9 @ . _ : -, starting with a
# letter or digit.
valid_unit() {
    case $1 in [A-Za-z0-9]*) ;; *) return 1 ;; esac
    case $1 in *[!A-Za-z0-9@._:-]*) return 1 ;; esac
    [ "${#1}" -le 128 ]
}

# A container name as Docker allows it: 1..128 of A-Z a-z 0-9 _ . -,
# starting with a letter or digit.
valid_container() {
    case $1 in [A-Za-z0-9]*) ;; *) return 1 ;; esac
    case $1 in *[!A-Za-z0-9_.-]*) return 1 ;; esac
    [ "${#1}" -le 128 ]
}

# A backup entry's name: 1..32 of a-z 0-9 -, starting with a letter or
# digit; "nylm" is nylm's own data.
valid_entry() {
    case $1 in [a-z0-9]*) ;; *) return 1 ;; esac
    case $1 in *[!a-z0-9-]*) return 1 ;; esac
    [ "${#1}" -le 32 ]
}

# An absolute path of A-Z a-z 0-9 / . _ -, not "/", without empty, "." or
# ".." parts and no trailing slash, at most 1024 bytes.
valid_path() {
    case $1 in /?*) ;; *) return 1 ;; esac
    case $1 in *[!A-Za-z0-9/._-]* | *//* | */ | */./* | */. | */../* | */..) return 1 ;; esac
    [ "${#1}" -le 1024 ]
}

# full_unit NAME: the unit's full name as systemd reads it, ".service"
# added when it has no unit type.
full_unit() {
    case $1 in
        ?*.service | ?*.socket | ?*.device | ?*.mount | ?*.automount | ?*.swap | ?*.target | \
            ?*.path | ?*.timer | ?*.slice | ?*.scope) printf '%s\n' "$1" ;;
        *) printf '%s.service\n' "$1" ;;
    esac
}

# unit_shown UNIT: nylm shows this unit's log (nylm's own, NYLM_UNITS, the
# backup job of an entry); UNIT is a full name.
unit_shown() {
    valid_unit "$1" || return 1
    for u in $NYLM_OWN_UNITS $NYLM_UNITS; do
        [ "$(full_unit "$u")" = "$1" ] && return 0
    done
    for e in $NYLM_BACKUP; do
        [ "nylm-backup@${e%%=*}.service" = "$1" ] && return 0
    done
    return 1
}

# entry_paths NAME: the paths of backup entry NAME, one per line. Fails if
# there is no such entry; dies if the entry is invalid.
entry_paths() {
    for e in $NYLM_BACKUP; do
        [ "${e%%=*}" = "$1" ] || continue
        valid_entry "$1" && [ "$1" != nylm ] || die "NYLM_BACKUP: invalid name: $1"
        case $e in
            *=*) ;;
            *) die "NYLM_BACKUP: $e is not name=/path[,/path...]" ;;
        esac
        case ${e#*=} in
            '' | ,* | *, | *,,*) die "NYLM_BACKUP: $1: an empty path" ;;
        esac
        old_ifs=$IFS
        IFS=,
        for p in ${e#*=}; do
            valid_path "$p" || die "NYLM_BACKUP: $1: invalid path: $p"
            printf '%s\n' "$p"
        done
        IFS=$old_ifs
        return 0
    done
    return 1
}

# Takes the jobs lock for the rest of the script, or dies.
take_lock() {
    exec 9<"$JOBS_LOCK"
    /usr/bin/flock -n 9 || die "another nylm job is running"
}
