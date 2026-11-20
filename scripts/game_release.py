"""GODZILLA's PS4 releases: their regions and the languages each one's game has.

The game takes its language from the PS4 system language (MT_LANGUAGE, a system language id): each
release's eboot.bin maps the ids it knows to its own text files and plays any other id in English.
- US (CUSA02046, 0x17eb50 in 01.01): 2 French, 17 Brazilian Portuguese, 20 Latin American
  Spanish (game_fr, game_bp, game_ns).
- Europe (CUSA02058, 0x17e210 in 01.00): 2 French, 3 Spanish, 4 German, 5 Italian (game_fr, game_es,
  game_de, game_it).
Both have the English text and the same English and Japanese voice files. The game stores the language in
its save data (+0x2a, the word both routines write) when it creates the save, and loading a save puts that one
back: MT_LANGUAGE is the language of a new save.
"""
ENGLISH = '1'
RELEASES = {
    'CUSA02046': ('US', (('English', '1'), ('Français', '2'), ('Español (Latinoamérica)', '20'),
                         ('Português (Brasil)', '17'))),
    'CUSA02058': ('Europe', (('English', '1'), ('Français', '2'), ('Deutsch', '4'), ('Español', '3'),
                             ('Italiano', '5'))),
}
# The release the port started with: its folders, saves and online copies come first.
DEFAULT_TITLE_ID = 'CUSA02046'
# A language one release has under another id in the other one (Spain's and Latin America's Spanish).
SAME_LANGUAGE = {'3': '20', '20': '3'}
# PS4 system language ids, for the log.
SYSTEM_LANGUAGES = {'0': 'Japanese', '1': 'English', '2': 'French', '3': 'Spanish', '4': 'German', '5': 'Italian',
                    '6': 'Dutch', '7': 'Portuguese', '8': 'Russian', '17': 'Brazilian Portuguese',
                    '18': 'English (UK)', '20': 'Latin American Spanish'}


def region(title_id):
    """The release's region ("Europe"), or None for a title ID that is not one of GODZILLA's known releases."""
    release = RELEASES.get(title_id or '')
    return release[0] if release else None


def languages(title_id):
    """(name, system language id) of the languages the release has; the first release's for an unknown ID."""
    return RELEASES.get(title_id or '', RELEASES[DEFAULT_TITLE_ID])[1]


def language_for(title_id, language):
    """The system language id to start the release with for the chosen one: the same language when the release
    has it (under its own id for Spanish), else English, which every release has."""
    ids = [value for _, value in languages(title_id)]
    language = str(language or ENGLISH)
    if language in ids:
        return language
    return SAME_LANGUAGE[language] if SAME_LANGUAGE.get(language) in ids else ENGLISH


def release_name(title_id):
    """"CUSA02058 (Europe)"; the title ID alone for a release that is not known."""
    return f'{title_id} ({region(title_id)})' if region(title_id) else title_id or '?'


def describe(title_id, version=None):
    """The release as the launcher names it: "CUSA02058 (Europe) · version 01.00"."""
    return f'{release_name(title_id)} · version {version or "?"}'


def log_line(sfo, language):
    """The log's line about the game: its release, its version and the language it starts in."""
    title_id = sfo.get('TITLE_ID')
    language = str(language or ENGLISH)
    text = f'{SYSTEM_LANGUAGES.get(language, "system language")} ({language})'
    if region(title_id) and language not in dict((v, n) for n, v in languages(title_id)):
        text += ', which this release does not have: its text is English'
    return f'Game: {sfo.get("TITLE", "?")} {release_name(title_id)}, version {sfo.get("APP_VER", "?")}, language {text}'
