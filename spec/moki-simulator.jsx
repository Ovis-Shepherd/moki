import { useState, useEffect } from 'react';

// ============================================================================
// MOKI — Device Simulator  ·  v0.3
// Target: LILYGO T5 E-Paper S3 Pro  ·  MeshCore on 868MHz  ·  30-min sync
//
// NEW IN v0.3:
//   · Habits now count multiple completions per day (GitHub-style heatmap)
//   · 12-week intensity grid on habit detail (5 shade levels)
//   · Todos added: categories, deadlines, recurring, descriptions
//   · "Tun" screen now has tabs: Gewohnheiten | Aufgaben
//   · Parametric Moki: ear/belly variants from pub_key seed
//   · Accessory system: earned items live in state, one can be worn
//
// PORTABILITY: all state keys match intended C struct fields for NVS.
// ============================================================================

// ----- Palette ---------------------------------------------------------------
const PAPER = '#e8e2d1';
const INK   = '#1a1612';
const DARK  = '#3a342c';
const MID   = '#8a8373';
const LIGHT = '#c9c1a8';

// ----- Screen IDs (mirror as C enum) -----------------------------------------
const S = {
  HOME: 'home', DO: 'do', HABIT_DETAIL: 'habit_detail', TODO_DETAIL: 'todo_detail',
  CAL: 'calendar', READ: 'read', READER: 'reader', FEED: 'feed', MAP: 'map',
  NEARBY: 'nearby', MOOD: 'mood', PROFILE: 'profile', MOKI: 'moki',
  CHATS: 'chats', CHAT: 'chat', FRIEND: 'friend',
  NOTE_NEW: 'note_new', NOTE_EDIT: 'note_edit',
  INBOX: 'inbox',
};

// ----- Mood presets (from user's spec) ---------------------------------------
const MOODS = [
  { id: 'camp',  icon: '⛺', label: 'campen',       hint: 'mal wieder raus' },
  { id: 'sport', icon: '⚡', label: 'sport',        hint: 'tischtennis · rad' },
  { id: 'food',  icon: '◒',  label: 'essen gehen',  hint: 'wer mag mit' },
  { id: 'drink', icon: '◉',  label: 'bierchen',     hint: 'einfach quatschen' },
  { id: 'spont', icon: '✦',  label: 'spontan',      hint: 'alles möglich' },
  { id: 'games', icon: '◈',  label: 'brettspiele',  hint: 'heute abend?' },
  { id: 'walk',  icon: '◐',  label: 'spazieren',    hint: 'mit gespräch' },
  { id: 'read',  icon: '☾',  label: 'still lesen',  hint: 'im café, gemeinsam' },
];

// ----- Todo categories — fixed set, earned by use ----------------------------
const CATEGORIES = {
  home:   { label: 'zuhause', mark: '◯' },
  plants: { label: 'pflanzen', mark: '◐' },
  work:   { label: 'arbeit',  mark: '◑' },
  self:   { label: 'selbst',  mark: '◉' },
  social: { label: 'freund_innen', mark: '◈' },
};

// ----- Moki accessories — unlocked by milestones -----------------------------
// Hardware: these are tiny SVG-rendered layers on top of base Moki bitmap.
const ACCESSORIES = {
  glasses: { label: 'lesebrille', unlock: 'nach 5 büchern gelesen'     },
  leaf:    { label: 'blatt',      unlock: '30 spaziergänge am stück'   },
  book:    { label: 'büchlein',   unlock: 'nach 100 habit-tagen'        },
  hat:     { label: 'mützchen',   unlock: '1 monat dabei'               },
  scarf:   { label: 'schal',      unlock: '10 mood-antworten geteilt'   },
};

// ============================================================================
// INITIAL STATE
// ============================================================================
function createInitialState() {
  return {
    pet: {
      name: 'moki',
      mood: 'calm',
      age_days: 14,
      streak: 3,
      // Parametric variant — derived from pub_key once, stored in NVS
      variant: { ears: 'tufted', belly: 'light' },
      worn: 'glasses',                      // one accessory can be worn
      earned: ['glasses', 'hat'],           // unlocked accessories
    },
    profile: {
      handle: 'levin',
      pub_key: 'HDB-3f2a',
      bio: 'liest langsam. läuft lieber.',
      visibility: 'friends',
    },
    // habits.history is now an array of COUNTS (0..N) per day, most recent last.
    // 84 days = 12 weeks of Git-style heatmap.
    habits: [
      { id: 1, name: 'Lesen · 10 min', todayCount: 1, streak: 4, history: genHistory(84, 0.65) },
      { id: 2, name: 'Spaziergang',    todayCount: 2, streak: 7, history: genHistory(84, 0.85) },
      { id: 3, name: 'Wasser',         todayCount: 3, streak: 2, history: genHistory(84, 0.9) },
      { id: 4, name: 'Tagebuch',       todayCount: 0, streak: 0, history: genHistory(84, 0.3) },
    ],
    todos: [
      { id: 1, title: 'Pflanzen gießen',     desc: 'monstera + basilikum', cat: 'plants', deadline: 'morgen',  recurring: 'weekly', done: false },
      { id: 2, title: 'Lina zurückrufen',    desc: 'wegen samstag',         cat: 'social', deadline: 'heute',   recurring: null,     done: false },
      { id: 3, title: 'Steuerkram sortieren', desc: '',                      cat: 'work',   deadline: 'diese woche', recurring: null, done: false },
      { id: 4, title: 'Küche wischen',       desc: '',                      cat: 'home',   deadline: null,       recurring: 'weekly', done: true  },
      { id: 5, title: 'zum arzt',            desc: 'vorsorge',              cat: 'self',   deadline: '28. apr', recurring: null,     done: false },
    ],
    reader: {
      book: 'Walden', author: 'Henry David Thoreau',
      page: 42, total: 312,
      excerpt:
        'Ich ging in die Wälder, weil ich mit Bedacht leben wollte, ' +
        'um nur den wesentlichen Tatsachen des Lebens ins Auge zu sehen, ' +
        'und zu lernen, was es zu lehren hatte — und nicht, wenn es zum ' +
        'Sterben käme, zu entdecken, dass ich nicht gelebt hatte.',
    },
    feed: [
      { id: 1, src: 'kottke.org',   title: 'The slow-reading revival',  unread: true,  date: 'gestern'    },
      { id: 2, src: 'craigmod.com', title: 'On walking and reading',    unread: true,  date: 'vorgestern' },
      { id: 3, src: 'aeon.co',      title: 'In praise of boredom',      unread: false, date: '3 Tage'     },
      { id: 4, src: 'nautil.us',    title: 'Letters to a young reader', unread: true,  date: '4 Tage'     },
    ],
    calendar: [
      { id: 1, day: 2, hour: '19:00', title: 'kochen mit lina',            place: 'zuhause',     kind: 'private' },
      { id: 2, day: 3, hour: '10:00', title: 'spaziergang philosophenweg', place: 'heidelberg',  kind: 'friends' },
      { id: 3, day: 5, hour: '20:30', title: 'lesekreis · walden',         place: 'café frieda', kind: 'public'  },
    ],
    nearby: [
      { id: 'moki-7f2a', name: 'lina', pub_key: 'HDB-7f2a',
        liked: 'The slow-reading revival', dist: '~80 m',  mood: 'drink',
        last_heard: 'im sync vor 3 min',
        bio: 'kocht gerne laut.', variant: { ears: 'round',   belly: 'light' }, worn: 'scarf',
        public_habits: [ { name: 'Yoga',  history: genHistory(84, 0.7) },
                          { name: 'Sketch', history: genHistory(84, 0.4) } ],
        public_books: ['Walden', 'Die Analphabetin', 'Stoner'],
      },
      { id: 'moki-a91c', name: 'tom',  pub_key: 'HDB-a91c',
        liked: 'Walden · Kap. 3', dist: '~200 m', mood: 'walk',
        last_heard: 'im sync vor 12 min',
        bio: 'jeden morgen am neckar.', variant: { ears: 'pointed', belly: 'dark' }, worn: 'hat',
        public_habits: [ { name: 'Laufen', history: genHistory(84, 0.9) } ],
        public_books: ['Im Grunde gut', 'Walden'],
      },
      { id: 'moki-c04e', name: 'juno', pub_key: 'HDB-c04e',
        liked: null, dist: '~450 m', mood: 'games',
        last_heard: 'im sync vor 2h',
        bio: 'bastelt. brettspielt. backt.', variant: { ears: 'tufted', belly: 'none' }, worn: null,
        public_habits: [ { name: 'Lesen', history: genHistory(84, 0.5) } ],
        public_books: ['Pachinko'],
      },
    ],
    // Chats: three kinds (direct / group / public). Groups may reset.
    chats: [
      { id: 'c1', kind: 'direct', name: 'lina', members: ['moki-7f2a'],
        last: 'magst du samstag tanzen?', ts: 'vor 8 min', unread: 1,
        reset: null,
        messages: [
          { from: 'lina', text: 'hey', ts: 'vor 22 min' },
          { from: 'lina', text: 'magst du samstag tanzen?', ts: 'vor 8 min' },
        ],
      },
      { id: 'c2', kind: 'group', name: 'lesekreis', members: ['moki-7f2a', 'moki-a91c'],
        last: 'walden kap 4 bis freitag ok?', ts: 'vor 2h', unread: 0,
        reset: 'weekly',
        messages: [
          { from: 'tom',  text: 'hat jemand das buch durch?', ts: 'vor 3h' },
          { from: 'lina', text: 'nein noch nicht', ts: 'vor 2h' },
          { from: 'lina', text: 'walden kap 4 bis freitag ok?', ts: 'vor 2h' },
        ],
      },
      { id: 'c3', kind: 'public', name: '#rhein-neckar', members: [],
        last: 'jemand heute abend am neckar?', ts: 'vor 45 min', unread: 3,
        reset: 'daily',
        messages: [
          { from: 'unbekannt · HDB-22ee', text: 'empfehlung café mit guter milch?', ts: 'vor 3h' },
          { from: 'juno',                 text: 'café frieda', ts: 'vor 2h' },
          { from: 'unbekannt · HDB-88dd', text: 'jemand heute abend am neckar?', ts: 'vor 45 min' },
        ],
      },
      { id: 'c4', kind: 'public', name: '#bücher', members: [],
        last: '— neu —', ts: 'still', unread: 0,
        reset: 'weekly',
        messages: [],
      },
    ],
    // Map: stylized local coords (0..100). Hardware: real lat/lng from GPS,
    // projected onto viewport centered on self.
    map: {
      // my own sharing mode: off | hourly | live
      share: 'hourly',
      self: { x: 50, y: 50 },
      places: [
        { id: 1, x: 38, y: 42, name: 'café frieda',      kind: 'saved' },
        { id: 2, x: 62, y: 30, name: 'zuhause',          kind: 'home'  },
        { id: 3, x: 72, y: 58, name: 'philosophenweg',   kind: 'saved' },
        { id: 4, x: 28, y: 68, name: 'schwimmbad',       kind: 'saved' },
      ],
      // friends: only those sharing live right now appear here
      friends: [
        { id: 'moki-7f2a', name: 'lina', x: 44, y: 46, fresh: '3 min' },
        { id: 'moki-a91c', name: 'tom',  x: 70, y: 62, fresh: '12 min' },
      ],
      // today's events with GPS coords
      events: [
        { id: 1, x: 62, y: 30, title: 'kochen mit lina', hour: '19:00' },
      ],
    },
    activeMood: null,
    // Notes — markdown body, template origin, visibility, pinned, folder
    notes: [
      { id: 'n1', title: 'Walden · Gedanken', template: 'idee', folder: 'gelesen',
        body: '# Walden\n\n> „Die meisten Menschen führen ein Leben stiller Verzweiflung."\n\n## was bleibt hängen\n- einfachheit als politische handlung\n- der **wald** als spiegel, nicht als flucht\n\n## nächste kapitel\n- kap. 4 bis freitag',
        updated_at: 'vor 2h', visibility: 'private', pinned: true },
      { id: 'n2', title: 'Miso-Ramen · einfach', template: 'rezept', folder: 'küche',
        body: '# miso-ramen\n\n*abends, für 2 personen*\n\n## zutaten\n- 400g ramen-nudeln\n- 3 el weißes miso\n- 1 l brühe\n- 2 lauchzwiebeln\n- 1 ei pro person\n- sesamöl\n\n## zubereitung\n1. eier 7 min kochen, kalt abschrecken\n2. brühe erhitzen, miso einrühren (**nicht kochen**)\n3. nudeln separat garen\n4. alles in schalen schichten\n\n## notiz\n- mit etwas chili-öl noch besser',
        updated_at: 'gestern', visibility: 'friends', pinned: false },
      { id: 'n3', title: 'dienstag', template: 'tagebuch', folder: 'tagebuch',
        body: '# dienstag · 20. april\n\n## war gut\n- lange am neckar gesessen\n- lina hat gelacht\n\n## gelernt\n- ich kann langsamer lesen\n\n## dankbar\n- für den kaffee heute morgen',
        updated_at: 'vor 4h', visibility: 'private', pinned: false },
    ],
    note_folders: ['tagebuch', 'küche', 'gelesen', 'ideen'],
    // Things handed to you by nearby Mokis — via BLE proximity handoff.
    // Kinds: note, book, place, recipe, habit-idea
    inbox: [
      { id: 'ix1', kind: 'note',   from: 'lina', at: 'gestern', preview: 'buchtipp · die unendliche geschichte' },
      { id: 'ix2', kind: 'place',  from: 'tom',  at: 'heute',   preview: 'stiller punkt am neckar' },
      { id: 'ix3', kind: 'recipe', from: 'juno', at: 'vor 2h',  preview: 'dinkel-pfannkuchen' },
    ],
    // Templates — user can add their own later
    note_templates: [
      { id: 'blank',  label: 'leer',     body: '' },
      { id: 'diary',  label: 'tagebuch', body: '# [datum]\n\n## war gut\n- \n\n## gelernt\n- \n\n## dankbar\n- ' },
      { id: 'recipe', label: 'rezept',   body: '# [name]\n\n*für [personen]*\n\n## zutaten\n- \n\n## zubereitung\n1. \n\n## notiz\n- ' },
      { id: 'list',   label: 'liste',    body: '# [titel]\n\n- \n- \n- ' },
      { id: 'idea',   label: 'idee',     body: '# [idee]\n\n> gedanke\n\n## warum\n\n## nächste schritte\n- ' },
    ],
    sync: {
      lastAt: Date.now() - 18 * 60_000,
      intervalSec: 30 * 60,
      roomServer: 'HDB-castle',
      signalDbm: -94,
    },
    battery: 78, wifi: false, lora: true, time: '14:32',
  };
}

// Generate plausibly varied count-history. intensity ≈ how diligent.
function genHistory(days, intensity) {
  const arr = []; let seed = 13;
  for (let i = 0; i < days; i++) {
    seed = (seed * 9301 + 49297) % 233280;
    const r = seed / 233280;
    if (r > intensity) { arr.push(0); continue; }
    const r2 = (seed * 17 + 31) % 233280 / 233280;
    if      (r2 < 0.55) arr.push(1);
    else if (r2 < 0.82) arr.push(2);
    else if (r2 < 0.95) arr.push(3);
    else                 arr.push(4);
  }
  return arr;
}

// Cell shade: 5 levels of ink, GitHub-style intensity
function cellStyle(count) {
  if (count === 0) return { background: 'rgba(0,0,0,0.055)' };
  if (count === 1) return { background: 'rgba(0,0,0,0.20)'  };
  if (count === 2) return { background: 'rgba(0,0,0,0.42)'  };
  if (count === 3) return { background: 'rgba(0,0,0,0.68)'  };
  return { background: INK };
}

// ============================================================================
// MOKI PET — now parametric + wearable accessories
// ============================================================================
function Moki({ size = 140, breathing = true, mood = 'calm', variant, worn }) {
  const v = variant || { ears: 'tufted', belly: 'light' };
  const eyesOpen = mood === 'happy' || mood === 'curious';
  return (
    <div style={{ animation: breathing ? 'moki-breathe 5s ease-in-out infinite' : 'none' }}>
      <svg width={size} height={size} viewBox="0 0 100 100" fill="none">
        {/* shadow */}
        <ellipse cx="50" cy="92" rx="26" ry="2" fill={LIGHT} />

        {/* EARS — variant */}
        {v.ears === 'pointed' && (
          <>
            <path d="M 28 42 Q 22 20 32 24 Q 36 36 36 44 Z" fill={INK} />
            <path d="M 72 42 Q 78 20 68 24 Q 64 36 64 44 Z" fill={INK} />
          </>
        )}
        {v.ears === 'round' && (
          <>
            <circle cx="32" cy="32" r="8" fill={INK} />
            <circle cx="68" cy="32" r="8" fill={INK} />
          </>
        )}
        {v.ears === 'tufted' && (
          <>
            <path d="M 28 42 Q 24 24 34 26 Q 36 36 36 44 Z" fill={INK} />
            <path d="M 72 42 Q 76 24 66 26 Q 64 36 64 44 Z" fill={INK} />
            <path d="M 30 36 Q 30 30 33 30" stroke={MID} strokeWidth="1.2" fill="none" />
            <path d="M 70 36 Q 70 30 67 30" stroke={MID} strokeWidth="1.2" fill="none" />
          </>
        )}

        {/* BODY */}
        <ellipse cx="50" cy="62" rx="28" ry="26" fill={INK} />

        {/* BELLY — variant */}
        {v.belly === 'light' && (
          <ellipse cx="50" cy="72" rx="14" ry="10" fill={DARK} opacity="0.55" />
        )}
        {v.belly === 'dark' && (
          <ellipse cx="50" cy="72" rx="16" ry="12" fill="#0a0907" />
        )}

        {/* CHEEKS */}
        <ellipse cx="38" cy="68" rx="4" ry="3" fill={DARK} opacity="0.6" />
        <ellipse cx="62" cy="68" rx="4" ry="3" fill={DARK} opacity="0.6" />

        {/* EYES */}
        {eyesOpen ? (
          <>
            <circle cx="42" cy="59" r="2.2" fill={PAPER} />
            <circle cx="58" cy="59" r="2.2" fill={PAPER} />
          </>
        ) : (
          <>
            <path d="M 38 60 Q 42 57 46 60" stroke={PAPER} strokeWidth="2" fill="none" strokeLinecap="round" />
            <path d="M 54 60 Q 58 57 62 60" stroke={PAPER} strokeWidth="2" fill="none" strokeLinecap="round" />
          </>
        )}

        {/* MOUTH */}
        <path d="M 47 70 Q 50 72 53 70" stroke={PAPER} strokeWidth="1.8" fill="none" strokeLinecap="round" />

        {/* PAWS */}
        <ellipse cx="40" cy="86" rx="6" ry="3.5" fill={INK} />
        <ellipse cx="60" cy="86" rx="6" ry="3.5" fill={INK} />

        {/* ACCESSORIES — overlaid */}
        {worn === 'glasses' && (
          <g stroke={PAPER} strokeWidth="1.3" fill="none">
            <circle cx="42" cy="59" r="4" />
            <circle cx="58" cy="59" r="4" />
            <line x1="46" y1="59" x2="54" y2="59" />
          </g>
        )}
        {worn === 'leaf' && (
          <path d="M 30 22 Q 26 14 34 14 Q 38 18 34 24 Z" fill={DARK} stroke={INK} strokeWidth="0.5" />
        )}
        {worn === 'hat' && (
          <>
            <path d="M 32 34 Q 50 22 68 34 L 66 40 Q 50 32 34 40 Z" fill={DARK} />
            <path d="M 28 40 L 72 40" stroke={INK} strokeWidth="1.5" />
          </>
        )}
        {worn === 'scarf' && (
          <>
            <path d="M 30 78 Q 50 84 70 78 L 70 82 Q 50 88 30 82 Z" fill={DARK} />
          </>
        )}
        {worn === 'book' && (
          <g>
            <rect x="24" y="88" width="12" height="8" fill={DARK} stroke={INK} strokeWidth="0.5" />
            <line x1="30" y1="88" x2="30" y2="96" stroke={INK} strokeWidth="0.5" />
          </g>
        )}
      </svg>
    </div>
  );
}

// ----- Icons -----------------------------------------------------------------
const I = {
  home:    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M3 11 L12 4 L21 11 V20 H3 Z"/><path d="M9 20 V14 H15 V20"/></svg>,
  task:    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M4 7 H20"/><path d="M4 12 H20"/><path d="M4 17 H14"/></svg>,
  cal:     <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><rect x="3" y="5" width="18" height="16" rx="1"/><path d="M3 10 H21"/><path d="M8 3 V7"/><path d="M16 3 V7"/></svg>,
  book:    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M4 5 Q4 3 6 3 H11 V21 H6 Q4 21 4 19 Z"/><path d="M20 5 Q20 3 18 3 H13 V21 H18 Q20 21 20 19 Z"/></svg>,
  rss:     <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M4 11 A9 9 0 0 1 13 20"/><path d="M4 4 A16 16 0 0 1 20 20"/><circle cx="5" cy="19" r="1.5" fill="currentColor"/></svg>,
  lora:    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M5 9 Q12 3 19 9"/><path d="M7 12 Q12 8 17 12"/><circle cx="12" cy="16" r="1.6" fill="currentColor"/></svg>,
  heart:   <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"><path d="M12 20 L4 12 Q0 6 6 4 Q10 4 12 8 Q14 4 18 4 Q24 6 20 12 Z"/></svg>,
  wifi:    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"><path d="M3 9 Q12 2 21 9"/><path d="M6 13 Q12 8 18 13"/><circle cx="12" cy="18" r="1.3" fill="currentColor"/></svg>,
  bolt:    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round"><rect x="4" y="8" width="15" height="8" rx="1"/><rect x="19" y="10" width="2" height="4" fill="currentColor"/></svg>,
  sync:    <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"><path d="M4 12 A8 8 0 0 1 18 7"/><path d="M18 3 V7 H14"/><path d="M20 12 A8 8 0 0 1 6 17"/><path d="M6 21 V17 H10"/></svg>,
  back:    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"><path d="M15 5 L8 12 L15 19"/></svg>,
  profile: <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21 Q4 14 12 14 Q20 14 20 21"/></svg>,
  recur:   <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M4 12 A8 8 0 0 1 18 7"/><path d="M18 3 V7 H14"/><path d="M20 12 A8 8 0 0 1 6 17"/><path d="M6 21 V17 H10"/></svg>,
  plus:    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"><path d="M12 5 V19"/><path d="M5 12 H19"/></svg>,
  map:     <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M3 7 L9 5 L15 7 L21 5 V17 L15 19 L9 17 L3 19 Z"/><path d="M9 5 V17"/><path d="M15 7 V19"/></svg>,
  chat:    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M4 5 H20 Q21 5 21 6 V16 Q21 17 20 17 H10 L5 21 V17 H4 Q3 17 3 16 V6 Q3 5 4 5 Z"/></svg>,
  send:    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"><path d="M3 12 L21 4 L14 21 L11 13 Z"/></svg>,
  share:   <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="10"/><path d="M12 8 V16"/><path d="M8 12 H16"/></svg>,
  inbox:   <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round"><path d="M3 13 V19 Q3 20 4 20 H20 Q21 20 21 19 V13"/><path d="M3 13 L6 5 H18 L21 13"/><path d="M3 13 H8 L10 15 H14 L16 13 H21"/></svg>,
};

// ============================================================================
// STATUS BAR + DOCK
// ============================================================================
function StatusBar({ state, onForceSync, syncing }) {
  const msSince = Date.now() - state.sync.lastAt;
  const secLeft = Math.max(0, state.sync.intervalSec - Math.floor(msSince / 1000));
  const minLeft = Math.ceil(secLeft / 60);
  return (
    <div className="flex items-center justify-between px-5 py-2 text-xs tracking-widest uppercase"
      style={{ fontFamily: '"JetBrains Mono", monospace', color: MID, borderBottom: `1px dashed ${LIGHT}` }}>
      <button onClick={onForceSync} className="flex items-center gap-1.5"
        style={{ color: syncing ? INK : DARK,
          animation: syncing ? 'sync-pulse 1.2s ease-in-out infinite' : 'none' }}>
        {I.sync}
        <span>{syncing ? 'synct …' : `sync · ${minLeft}m`}</span>
      </button>
      <span className="flex items-center gap-3" style={{ color: DARK }}>
        {state.lora && <span className="flex items-center gap-1">{I.lora}</span>}
        {state.wifi && <span className="flex items-center gap-1">{I.wifi}</span>}
        <span className="flex items-center gap-1">{I.bolt} {state.battery}</span>
        <span>{state.time}</span>
      </span>
    </div>
  );
}

function Dock({ current, onSelect }) {
  const items = [
    { id: S.HOME,   label: 'heim',   icon: I.home },
    { id: S.DO,     label: 'tun',    icon: I.task },
    { id: S.READ,   label: 'lesen',  icon: I.book },
    { id: S.CHATS,  label: 'chat',   icon: I.chat },
    { id: S.MAP,    label: 'karte',  icon: I.map  },
  ];
  return (
    <div className="flex items-center justify-around py-1.5"
      style={{ borderTop: `1px solid ${LIGHT}`, background: PAPER }}>
      {items.map(it => {
        const active = current === it.id;
        return (
          <button key={it.id} onClick={() => onSelect(it.id)}
            className="flex flex-col items-center gap-0.5 px-1 py-1"
            style={{ color: active ? INK : MID,
              fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
              letterSpacing: '0.08em', opacity: active ? 1 : 0.55 }}>
            <div style={{ borderBottom: active ? `1.5px solid ${INK}` : 'none', paddingBottom: 2 }}>
              {it.icon}
            </div>
            <span>{it.label}</span>
          </button>
        );
      })}
    </div>
  );
}

// ----- Shared bits -----------------------------------------------------------
function Title({ kicker, title }) {
  return (
    <div>
      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>{kicker}</p>
      <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, fontWeight: 400,
        fontStyle: 'italic', color: INK, marginTop: 2 }}>{title}</h2>
    </div>
  );
}
function BackBar({ onBack, label }) {
  return (
    <button onClick={onBack} className="flex items-center gap-2 py-1"
      style={{ color: MID, fontFamily: '"JetBrains Mono", monospace',
        fontSize: 10, letterSpacing: '0.2em', textTransform: 'uppercase' }}>
      {I.back} <span>{label}</span>
    </button>
  );
}
function Stat({ label, value, sub, onClick }) {
  return (
    <button onClick={onClick} className="flex flex-col items-center py-1.5"
      style={{ border: `1px solid ${LIGHT}`, borderRadius: 2, background: PAPER }}>
      <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
        letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>{label}</span>
      <span style={{ fontFamily: '"Fraunces", serif', fontSize: 20, fontWeight: 500, color: INK, marginTop: 1 }}>
        {value}
      </span>
      <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
        letterSpacing: '0.12em', color: MID }}>{sub}</span>
    </button>
  );
}

// ============================================================================
// HOME
// ============================================================================
function HomeScreen({ state, go }) {
  const habitsDone = state.habits.filter(h => h.todayCount > 0).length;
  const openTodos = state.todos.filter(t => !t.done).length;
  const inboxCount = state.inbox.length;
  return (
    <div className="flex-1 flex flex-col px-7 py-5" style={{ background: PAPER }}>
      <div className="flex items-start justify-between w-full">
        <div>
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>
            dienstag · 20. april
          </p>
          <h1 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, fontWeight: 300,
            fontStyle: 'italic', color: INK, marginTop: 4, lineHeight: 1.2, maxWidth: 260 }}>
            langsam, aber jeden tag.
          </h1>
        </div>
        <button onClick={() => go(S.PROFILE)}
          style={{ border: `1.2px solid ${INK}`, borderRadius: '50%',
            width: 34, height: 34, display: 'flex', alignItems: 'center',
            justifyContent: 'center', color: INK, flexShrink: 0 }}>
          {I.profile}
        </button>
      </div>

      <button onClick={() => go(S.MOKI)} className="flex flex-col items-center my-3">
        <Moki size={130} variant={state.pet.variant} worn={state.pet.worn} />
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 17, color: INK, marginTop: 4, fontWeight: 500 }}>
          moki
        </p>
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.18em', color: MID, textTransform: 'uppercase' }}>
          tag {state.pet.age_days} · {state.pet.streak} in folge
        </p>
      </button>

      <button onClick={() => go(S.MOOD)}
        style={{ border: `1px dashed ${DARK}`, padding: '10px 14px', borderRadius: 2,
          background: state.activeMood ? INK : 'transparent',
          color: state.activeMood ? PAPER : DARK,
          fontFamily: '"Fraunces", serif', fontSize: 14, fontStyle: 'italic',
          display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <span>
          {state.activeMood
            ? `du teilst: ${MOODS.find(m => m.id === state.activeMood.id)?.label}`
            : 'wie fühlst du dich heute?'}
        </span>
        <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          {state.activeMood ? 'aktiv' : 'teilen →'}
        </span>
      </button>

      <div className="grid grid-cols-3 gap-2 mt-3">
        <Stat label="gewohn" value={`${habitsDone}/${state.habits.length}`} sub="heute" onClick={() => go(S.DO)} />
        <Stat label="aufgab" value={openTodos}                              sub="offen" onClick={() => go(S.DO)} />
        <Stat label="nah"    value={state.nearby.length}                    sub="in der nähe" onClick={() => go(S.MAP)} />
      </div>

      {inboxCount > 0 && (
        <button onClick={() => go(S.INBOX)}
          className="flex items-center justify-between mt-3 px-4 py-3"
          style={{ background: INK, color: PAPER, borderRadius: 2 }}>
          <div className="flex items-center gap-2.5">
            <span style={{ color: PAPER }}>{I.inbox}</span>
            <div className="text-left">
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 14,
                fontStyle: 'italic' }}>
                {inboxCount} {inboxCount === 1 ? 'ding wartet' : 'dinge warten'}
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                letterSpacing: '0.15em', color: LIGHT, textTransform: 'uppercase',
                marginTop: 1 }}>
                in der sammelbox
              </div>
            </div>
          </div>
          <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
            öffnen →
          </span>
        </button>
      )}
    </div>
  );
}

// ============================================================================
// DO SCREEN — tabs: Gewohnheiten | Aufgaben
// ============================================================================
function DoScreen({ state, onIncrementHabit, onOpenHabit, onToggleTodo, onAddTodo, onAddHabit, onAddEvent, tab, setTab }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      {/* Three-tab switcher */}
      <div className="flex" style={{ border: `1px solid ${INK}`, borderRadius: 2, padding: 2, marginBottom: 14 }}>
        <TabBtn label="gewohnheiten" active={tab === 'habits'}   onClick={() => setTab('habits')}   />
        <TabBtn label="aufgaben"     active={tab === 'todos'}    onClick={() => setTab('todos')}    />
        <TabBtn label="kalender"     active={tab === 'calendar'} onClick={() => setTab('calendar')} />
      </div>

      {tab === 'habits'   && <HabitList habits={state.habits} onIncrement={onIncrementHabit} onOpen={onOpenHabit} onAdd={onAddHabit} />}
      {tab === 'todos'    && <TodoList todos={state.todos} onToggle={onToggleTodo} onAdd={onAddTodo} />}
      {tab === 'calendar' && <CalendarInline state={state} onAddEvent={onAddEvent} />}
    </div>
  );
}

// Calendar content extracted so it can live inside the Do tabs
function CalendarInline({ state, onAddEvent }) {
  const today = 2;
  const week = ['m', 'd', 'm', 'd', 'f', 's', 's'];
  const dates = [19, 20, 21, 22, 23, 24, 25];
  const iconFor = (kind) => kind === 'public' ? '◉' : kind === 'friends' ? '◐' : '◯';
  return (
    <div className="flex-1 flex flex-col">
      <div className="flex justify-between mb-4 gap-1">
        {week.map((w, i) => {
          const active = i === today;
          const hasEvent = state.calendar.some(e => e.day === i);
          return (
            <button key={i} className="flex flex-col items-center gap-1 flex-1 py-2"
              style={{ background: active ? INK : 'transparent', borderRadius: 2 }}>
              <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.15em', color: active ? LIGHT : MID, textTransform: 'uppercase' }}>
                {w}
              </span>
              <span style={{ fontFamily: '"Fraunces", serif', fontSize: 18,
                color: active ? PAPER : INK, fontWeight: 500 }}>
                {dates[i]}
              </span>
              {hasEvent && <span style={{ width: 4, height: 4, borderRadius: '50%',
                background: active ? PAPER : INK }} />}
            </button>
          );
        })}
      </div>

      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 6 }}>
        kommend
      </p>
      <div className="flex-1">
        {state.calendar.map(ev => (
          <div key={ev.id} className="py-3 flex items-start gap-3"
            style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            <div style={{ textAlign: 'center', minWidth: 40 }}>
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 16, color: INK, fontWeight: 500 }}>
                {dates[ev.day]}
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.1em', color: MID }}>{ev.hour}</div>
            </div>
            <div className="flex-1">
              <div className="flex items-center gap-1.5">
                <span style={{ color: DARK, fontSize: 11 }}>{iconFor(ev.kind)}</span>
                <span style={{ fontFamily: '"Fraunces", serif', fontSize: 15,
                  color: INK, fontWeight: 500 }}>{ev.title}</span>
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.15em', color: MID, marginTop: 2, textTransform: 'uppercase' }}>
                {ev.place} · {ev.kind === 'public' ? 'öffentlich' : ev.kind === 'friends' ? 'freund_innen' : 'privat'}
              </div>
            </div>
          </div>
        ))}
      </div>

      <button onClick={onAddEvent}
        style={{ marginTop: 8, padding: '10px', border: `1px dashed ${MID}`,
          color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase',
          display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
        {I.plus} termin
      </button>
    </div>
  );
}

function TabBtn({ label, active, onClick }) {
  return (
    <button onClick={onClick} className="flex-1 py-2"
      style={{ background: active ? INK : 'transparent',
        color: active ? PAPER : MID,
        fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', textTransform: 'uppercase',
        borderRadius: 1 }}>
      {label}
    </button>
  );
}

// ----- HABIT LIST — tap name = +1, tap pill = detail ------------------------
function HabitList({ habits, onIncrement, onOpen, onAdd }) {
  return (
    <div className="flex-1 flex flex-col gap-1">
      {habits.map(h => (
        <div key={h.id} className="flex items-center justify-between py-2.5 gap-2"
          style={{ borderBottom: `1px dotted ${LIGHT}` }}>
          <button onClick={() => onIncrement(h.id)} className="flex-1 text-left">
            <div style={{ fontFamily: '"Fraunces", serif', fontSize: 16, color: INK }}>
              {h.name}
            </div>
            <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
              letterSpacing: '0.15em', color: MID, marginTop: 1, textTransform: 'uppercase' }}>
              {h.streak > 0 ? `${h.streak} tage in folge` : 'noch keine serie'}
            </div>
          </button>
          <button onClick={() => onOpen(h.id)}
            className="flex items-center gap-2 px-3 py-2"
            style={{ border: `1px solid ${h.todayCount > 0 ? INK : LIGHT}`,
              background: h.todayCount > 0 ? INK : 'transparent',
              color: h.todayCount > 0 ? PAPER : MID, borderRadius: 2 }}>
            <span style={{ fontFamily: '"Fraunces", serif', fontSize: 15, fontWeight: 500 }}>
              {h.todayCount}
            </span>
            <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
              letterSpacing: '0.15em', textTransform: 'uppercase' }}>
              × heute →
            </span>
          </button>
        </div>
      ))}
      <button onClick={onAdd}
        style={{ marginTop: 8, padding: '10px', border: `1px dashed ${MID}`,
        color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', textTransform: 'uppercase',
        display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
        {I.plus} neue gewohnheit
      </button>
    </div>
  );
}

// ----- TODO LIST — checkboxes + categories + deadlines + recurring ----------
function TodoList({ todos, onToggle, onAdd }) {
  // Group by done / open
  const open = todos.filter(t => !t.done);
  const done = todos.filter(t => t.done);
  return (
    <div className="flex-1 flex flex-col">
      <TodoItems items={open} onToggle={onToggle} />
      {done.length > 0 && (
        <>
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase',
            marginTop: 14, marginBottom: 4 }}>erledigt</p>
          <TodoItems items={done} onToggle={onToggle} faded />
        </>
      )}
      <button onClick={onAdd}
        style={{ marginTop: 'auto', padding: '10px', border: `1px dashed ${MID}`,
        color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', textTransform: 'uppercase',
        display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
        {I.plus} neue aufgabe
      </button>
    </div>
  );
}

function TodoItems({ items, onToggle, faded }) {
  return (
    <div className="flex flex-col">
      {items.map(t => {
        const cat = CATEGORIES[t.cat];
        return (
          <div key={t.id} className="flex items-start gap-3 py-2.5"
            style={{ borderBottom: `1px dotted ${LIGHT}`, opacity: faded ? 0.55 : 1 }}>
            <button onClick={() => onToggle(t.id)}
              style={{ width: 18, height: 18, border: `1.6px solid ${INK}`,
                background: t.done ? INK : 'transparent',
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                flexShrink: 0, marginTop: 2 }}>
              {t.done && <span style={{ color: PAPER, fontSize: 11, lineHeight: 1 }}>✓</span>}
            </button>
            <div className="flex-1">
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 15, color: INK,
                textDecoration: t.done ? 'line-through' : 'none', lineHeight: 1.3 }}>
                {t.title}
              </div>
              {t.desc && (
                <div style={{ fontFamily: '"Fraunces", serif', fontSize: 12, color: DARK,
                  fontStyle: 'italic', marginTop: 2, opacity: 0.7 }}>
                  {t.desc}
                </div>
              )}
              <div className="flex items-center gap-2 mt-1.5"
                style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                  letterSpacing: '0.12em', color: MID, textTransform: 'uppercase' }}>
                {cat && <span>{cat.mark} {cat.label}</span>}
                {t.deadline && <span>· bis {t.deadline}</span>}
                {t.recurring && (
                  <span className="inline-flex items-center gap-0.5">
                    · {I.recur} {t.recurring === 'weekly' ? 'wöchentlich' : 'täglich'}
                  </span>
                )}
              </div>
            </div>
          </div>
        );
      })}
    </div>
  );
}

// ============================================================================
// HABIT DETAIL — GitHub-style 12-week intensity heatmap
// ============================================================================
function HabitDetailScreen({ state, habitId, onBack }) {
  const h = state.habits.find(x => x.id === habitId);
  if (!h) return null;
  const WEEKS = 12;
  const DAYS = 7;
  const days = h.history;                              // 84 cells, most recent LAST
  const total = days.reduce((s, v) => s + v, 0);
  const activeDays = days.filter(d => d > 0).length;
  const rate = Math.round((activeDays / days.length) * 100);

  // Month labels for columns — approximate, assumes today is last cell
  const monthLabels = ['feb', 'mär', 'apr'];

  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />
      <div className="mt-1">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>gewohnheit</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontStyle: 'italic', marginTop: 2 }}>{h.name}</h2>
      </div>

      {/* MONTH LABELS — roughly aligned above grid columns */}
      <div className="mt-5" style={{ paddingLeft: 26 }}>
        <div className="flex justify-between"
          style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.15em', color: MID, textTransform: 'uppercase', marginBottom: 4 }}>
          {monthLabels.map((m, i) => <span key={i}>{m}</span>)}
        </div>

        {/* GRID with weekday labels on left */}
        <div className="flex gap-1.5">
          {/* weekday labels */}
          <div className="flex flex-col justify-between" style={{ fontFamily: '"JetBrains Mono", monospace',
            fontSize: 8, letterSpacing: '0.1em', color: MID, textTransform: 'uppercase',
            paddingTop: 2, paddingBottom: 2 }}>
            <span>mo</span>
            <span style={{ opacity: 0 }}>.</span>
            <span>mi</span>
            <span style={{ opacity: 0 }}>.</span>
            <span>fr</span>
            <span style={{ opacity: 0 }}>.</span>
            <span style={{ opacity: 0 }}>.</span>
          </div>
          {/* cell grid */}
          <div style={{ display: 'grid', gridTemplateColumns: `repeat(${WEEKS}, 1fr)`,
            gap: 2.5, flex: 1 }}>
            {Array.from({ length: WEEKS }).map((_, w) => (
              <div key={w} style={{ display: 'flex', flexDirection: 'column', gap: 2.5 }}>
                {Array.from({ length: DAYS }).map((_, d) => {
                  const idx = w * DAYS + d;
                  const count = days[idx] || 0;
                  return (
                    <div key={d} style={{
                      aspectRatio: '1/1',
                      ...cellStyle(count),
                      borderRadius: 1.5,
                    }} />
                  );
                })}
              </div>
            ))}
          </div>
        </div>

        {/* Legend */}
        <div className="flex items-center justify-end gap-1.5 mt-3"
          style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
            letterSpacing: '0.12em', color: MID, textTransform: 'uppercase' }}>
          <span>weniger</span>
          {[0, 1, 2, 3, 4].map(n => (
            <div key={n} style={{ width: 10, height: 10, ...cellStyle(n), borderRadius: 1.5 }} />
          ))}
          <span>mehr</span>
        </div>
      </div>

      {/* Stats */}
      <div className="grid grid-cols-3 gap-2 mt-6">
        <Stat label="heute"   value={`${h.todayCount}×`}     sub="gemacht" />
        <Stat label="gesamt"  value={total}                  sub="mal in 12 wo." />
        <Stat label="serie"   value={h.streak}               sub="tage am stück" />
      </div>

      <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, fontStyle: 'italic',
        color: DARK, marginTop: 'auto', textAlign: 'center', opacity: 0.7, lineHeight: 1.5 }}>
        „kleine sachen, oft.<br/>das brennt ein."
      </p>
    </div>
  );
}

// ============================================================================
// CALENDAR  (unchanged from v0.2)
// ============================================================================
function CalendarScreen({ state, onAddEvent }) {
  const today = 2;
  const week = ['m', 'd', 'm', 'd', 'f', 's', 's'];
  const dates = [19, 20, 21, 22, 23, 24, 25];
  const iconFor = (kind) => kind === 'public' ? '◉' : kind === 'friends' ? '◐' : '◯';
  return (
    <div className="flex-1 flex flex-col px-7 py-5" style={{ background: PAPER }}>
      <Title kicker="april" title="diese woche." />

      <div className="flex justify-between mt-5 mb-5 gap-1">
        {week.map((w, i) => {
          const active = i === today;
          const hasEvent = state.calendar.some(e => e.day === i);
          return (
            <button key={i} className="flex flex-col items-center gap-1 flex-1 py-2"
              style={{ background: active ? INK : 'transparent', borderRadius: 2 }}>
              <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.15em', color: active ? LIGHT : MID, textTransform: 'uppercase' }}>
                {w}
              </span>
              <span style={{ fontFamily: '"Fraunces", serif', fontSize: 18,
                color: active ? PAPER : INK, fontWeight: 500 }}>
                {dates[i]}
              </span>
              {hasEvent && <span style={{ width: 4, height: 4, borderRadius: '50%',
                background: active ? PAPER : INK }} />}
            </button>
          );
        })}
      </div>

      <div className="flex-1 flex flex-col">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 8 }}>
          kommend
        </p>
        {state.calendar.map(ev => (
          <div key={ev.id} className="py-3 flex items-start gap-3"
            style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            <div style={{ textAlign: 'center', minWidth: 40 }}>
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 16, color: INK, fontWeight: 500 }}>
                {dates[ev.day]}
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.1em', color: MID }}>{ev.hour}</div>
            </div>
            <div className="flex-1">
              <div className="flex items-center gap-1.5">
                <span style={{ color: DARK, fontSize: 11 }}>{iconFor(ev.kind)}</span>
                <span style={{ fontFamily: '"Fraunces", serif', fontSize: 15,
                  color: INK, fontWeight: 500 }}>{ev.title}</span>
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.15em', color: MID, marginTop: 2, textTransform: 'uppercase' }}>
                {ev.place} · {ev.kind === 'public' ? 'öffentlich' : ev.kind === 'friends' ? 'freund_innen' : 'privat'}
              </div>
            </div>
          </div>
        ))}
      </div>

      <button onClick={onAddEvent}
        style={{ marginTop: 6, padding: '10px', border: `1px dashed ${MID}`,
        color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        + termin
      </button>
    </div>
  );
}

// ============================================================================
// READ — tabs: Buch | Feed
// ============================================================================
function ReadScreen({ state, tab, setTab, onOpenNote, onNewNote, onToggleNotePin, folderFilter, setFolderFilter, onHandoffBook }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <div className="flex" style={{ border: `1px solid ${INK}`, borderRadius: 2, padding: 2, marginBottom: 14 }}>
        <TabBtn label="buch"    active={tab === 'book'}  onClick={() => setTab('book')}  />
        <TabBtn label="feed"    active={tab === 'feed'}  onClick={() => setTab('feed')}  />
        <TabBtn label="notizen" active={tab === 'notes'} onClick={() => setTab('notes')} />
      </div>
      {tab === 'book'  && <BookTab state={state} onHandoff={onHandoffBook} />}
      {tab === 'feed'  && <FeedTab state={state} />}
      {tab === 'notes' && <NotesTab state={state} onOpenNote={onOpenNote} onNewNote={onNewNote}
                             onTogglePin={onToggleNotePin}
                             folderFilter={folderFilter} setFolderFilter={setFolderFilter} />}
    </div>
  );
}

function BookTab({ state, onHandoff }) {
  const r = state.reader;
  return (
    <div className="flex-1 flex flex-col">
      <div style={{ textAlign: 'center' }}>
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>{r.author}</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, fontStyle: 'italic',
          color: INK, marginTop: 2 }}>{r.book}</h2>
      </div>
      <div className="flex-1 flex items-center justify-center mt-4 mb-2">
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14.5, lineHeight: 1.65,
          color: DARK, fontWeight: 400, textAlign: 'justify', hyphens: 'auto', maxWidth: 340 }}>
          {r.excerpt}
        </p>
      </div>
      <div className="flex items-center justify-between pt-3"
        style={{ borderTop: `1px dotted ${LIGHT}`, fontFamily: '"JetBrains Mono", monospace',
          fontSize: 10, letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>
        <span>← zurück</span>
        <span>{r.page} / {r.total}</span>
        <span>weiter →</span>
      </div>
      <button onClick={() => onHandoff && onHandoff(r)}
        className="flex items-center justify-center gap-2 mt-3 py-2.5"
        style={{ border: `1px dashed ${DARK}`, color: DARK,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        {I.share} buch empfehlen
      </button>
    </div>
  );
}

function FeedTab({ state }) {
  return (
    <div className="flex-1 flex flex-col">
      {state.feed.map(item => (
        <div key={item.id} className="py-3" style={{ borderBottom: `1px dotted ${LIGHT}` }}>
          <div className="flex items-center justify-between mb-1">
            <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
              letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>{item.src}</span>
            <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
              letterSpacing: '0.1em', color: MID }}>{item.date}</span>
          </div>
          <div className="flex items-start gap-2">
            {item.unread && <span style={{ color: INK, fontSize: 16, lineHeight: 1 }}>•</span>}
            <p style={{ fontFamily: '"Fraunces", serif', fontSize: 15, color: INK,
              fontStyle: item.unread ? 'normal' : 'italic', opacity: item.unread ? 1 : 0.55,
              fontWeight: item.unread ? 500 : 400 }}>{item.title}</p>
          </div>
        </div>
      ))}
    </div>
  );
}

// ============================================================================
// MARKDOWN — tiny line-based renderer for our subset
//   # H1 · ## H2 · ### H3 · - bullet · 1. numbered · - [ ] todo · - [x] done
//   > quote · --- hr · **bold** · *italic*  ·  blank = paragraph break
// Hardware: LVGL draws this line by line into the canvas area.
// ============================================================================
function renderInline(text) {
  if (!text) return text;
  const segs = text.split(/(\*\*[^*\n]+\*\*|\*[^*\n]+\*)/g).filter(s => s.length > 0);
  return segs.map((s, i) => {
    if (s.startsWith('**') && s.endsWith('**'))
      return <strong key={i} style={{ fontWeight: 600 }}>{s.slice(2, -2)}</strong>;
    if (s.startsWith('*') && s.endsWith('*') && s.length > 2)
      return <em key={i} style={{ fontStyle: 'italic' }}>{s.slice(1, -1)}</em>;
    return <span key={i}>{s}</span>;
  });
}

function Markdown({ text }) {
  const lines = (text || '').split('\n');
  return (
    <div className="flex flex-col">
      {lines.map((line, i) => {
        // h1
        if (line.startsWith('# '))
          return <h1 key={i} style={{ fontFamily: '"Fraunces", serif', fontSize: 22,
            fontWeight: 500, color: INK, marginTop: i === 0 ? 0 : 10, marginBottom: 6,
            lineHeight: 1.25 }}>{renderInline(line.slice(2))}</h1>;
        // h2
        if (line.startsWith('## '))
          return <h2 key={i} style={{ fontFamily: '"Fraunces", serif', fontSize: 16,
            fontStyle: 'italic', fontWeight: 500, color: INK,
            marginTop: 12, marginBottom: 4 }}>{renderInline(line.slice(3))}</h2>;
        // h3
        if (line.startsWith('### '))
          return <h3 key={i} style={{ fontFamily: '"Fraunces", serif', fontSize: 14,
            fontStyle: 'italic', color: DARK,
            marginTop: 10, marginBottom: 2 }}>{renderInline(line.slice(4))}</h3>;
        // task
        if (line.startsWith('- [ ] '))
          return <div key={i} className="flex items-start gap-2 py-0.5">
            <div style={{ width: 13, height: 13, border: `1.4px solid ${INK}`,
              marginTop: 4, flexShrink: 0 }} />
            <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: DARK,
              lineHeight: 1.55 }}>{renderInline(line.slice(6))}</span>
          </div>;
        if (line.startsWith('- [x] '))
          return <div key={i} className="flex items-start gap-2 py-0.5">
            <div style={{ width: 13, height: 13, background: INK, marginTop: 4,
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              flexShrink: 0 }}>
              <span style={{ color: PAPER, fontSize: 9, lineHeight: 1 }}>✓</span>
            </div>
            <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: MID,
              textDecoration: 'line-through', lineHeight: 1.55 }}>
              {renderInline(line.slice(6))}
            </span>
          </div>;
        // bullet
        if (line.startsWith('- '))
          return <div key={i} className="flex items-start gap-2 py-0.5">
            <span style={{ color: DARK, fontSize: 14, lineHeight: 1.55, flexShrink: 0 }}>·</span>
            <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: DARK,
              lineHeight: 1.55 }}>{renderInline(line.slice(2))}</span>
          </div>;
        // numbered
        const numMatch = line.match(/^(\d+)\. (.*)$/);
        if (numMatch)
          return <div key={i} className="flex items-start gap-2 py-0.5">
            <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 12,
              color: MID, lineHeight: 1.55, flexShrink: 0, minWidth: 16 }}>
              {numMatch[1]}.
            </span>
            <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: DARK,
              lineHeight: 1.55 }}>{renderInline(numMatch[2])}</span>
          </div>;
        // quote
        if (line.startsWith('> '))
          return <div key={i} style={{ borderLeft: `2px solid ${MID}`, paddingLeft: 10,
            paddingTop: 3, paddingBottom: 3, marginTop: 4, marginBottom: 4,
            fontFamily: '"Fraunces", serif', fontSize: 13.5, fontStyle: 'italic',
            color: DARK, lineHeight: 1.5, opacity: 0.85 }}>
            {renderInline(line.slice(2))}
          </div>;
        // hr
        if (line.trim() === '---')
          return <div key={i} style={{ borderTop: `1px dashed ${LIGHT}`,
            marginTop: 10, marginBottom: 10 }} />;
        // empty
        if (line.trim() === '')
          return <div key={i} style={{ height: 8 }} />;
        // paragraph
        return <p key={i} style={{ fontFamily: '"Fraunces", serif', fontSize: 14,
          color: DARK, lineHeight: 1.6, marginTop: 0, marginBottom: 0, paddingTop: 2,
          paddingBottom: 2 }}>{renderInline(line)}</p>;
      })}
    </div>
  );
}

// ============================================================================
// NOTES TAB — list view inside Read screen
// ============================================================================
function NotesTab({ state, onOpenNote, onNewNote, onTogglePin, folderFilter, setFolderFilter }) {
  const visLabel = { private: '◯', friends: '◐', public: '◉' };
  const visible = folderFilter === 'alle'
    ? state.notes
    : state.notes.filter(n => n.folder === folderFilter);
  const pinned = visible.filter(n => n.pinned);
  const rest   = visible.filter(n => !n.pinned);
  const ordered = [...pinned, ...rest];

  return (
    <div className="flex-1 flex flex-col">
      {/* Folder filter chips */}
      <div className="flex gap-1 mb-3 flex-wrap">
        <FolderChip label="alle" active={folderFilter === 'alle'}
          onClick={() => setFolderFilter('alle')} />
        {state.note_folders.map(f => (
          <FolderChip key={f} label={f} active={folderFilter === f}
            onClick={() => setFolderFilter(f)} />
        ))}
      </div>

      <div className="flex-1 flex flex-col overflow-y-auto">
        {ordered.length === 0 && (
          <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: MID,
            fontStyle: 'italic', textAlign: 'center', marginTop: 30, opacity: 0.65 }}>
            in „{folderFilter}" ist noch nichts.
          </p>
        )}
        {ordered.map(n => (
          <button key={n.id} onClick={() => onOpenNote(n.id)}
            className="py-3 text-left"
            style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            <div className="flex items-center justify-between gap-2">
              <div className="flex items-center gap-1.5 flex-1 min-w-0">
                {n.pinned && <span style={{ color: INK, fontSize: 10, flexShrink: 0 }}>★</span>}
                <span style={{ fontFamily: '"Fraunces", serif', fontSize: 15,
                  color: INK, fontWeight: 500,
                  whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                  {n.title || '— ohne titel —'}
                </span>
              </div>
              <span style={{ color: DARK, fontSize: 11, flexShrink: 0 }}>
                {visLabel[n.visibility]}
              </span>
            </div>
            <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
              letterSpacing: '0.15em', color: MID, marginTop: 2,
              textTransform: 'uppercase' }}>
              {n.folder ? `${n.folder} · ` : ''}{n.template} · {n.updated_at}
            </div>
          </button>
        ))}
      </div>

      <button onClick={onNewNote}
        style={{ marginTop: 10, padding: '10px', border: `1px dashed ${MID}`,
          color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase',
          display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
        {I.plus} neue notiz
      </button>
    </div>
  );
}

function FolderChip({ label, active, onClick }) {
  return (
    <button onClick={onClick}
      style={{ padding: '4px 9px', borderRadius: 2,
        border: `1px ${active ? 'solid' : 'dashed'} ${active ? INK : LIGHT}`,
        background: active ? INK : 'transparent',
        color: active ? PAPER : DARK,
        fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.15em', textTransform: 'uppercase' }}>
      {label}
    </button>
  );
}

// ============================================================================
// TEMPLATE PICKER — full screen when creating a new note
// ============================================================================
function TemplatePickerScreen({ state, onPick, onPickNew, onBack }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />
      <div className="mt-1">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>neu</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontStyle: 'italic', marginTop: 2 }}>was wird es?</h2>
      </div>

      <div className="grid grid-cols-2 gap-2 mt-5">
        {state.note_templates.map(t => (
          <button key={t.id} onClick={() => onPick(t.id)}
            style={{ padding: '12px 10px', textAlign: 'left',
              border: `1px dashed ${LIGHT}`, borderRadius: 2, background: 'transparent' }}>
            <div style={{ fontFamily: '"Fraunces", serif', fontSize: 16,
              fontStyle: 'italic', color: INK, marginBottom: 4 }}>
              {t.label}
            </div>
            <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
              letterSpacing: '0.1em', color: MID, textTransform: 'uppercase',
              lineHeight: 1.5,
              whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
              {t.body.split('\n')[0].slice(0, 22) || '(leer)'}
            </div>
          </button>
        ))}
        <button onClick={onPickNew}
          style={{ padding: '12px 10px', textAlign: 'left',
            border: `1.5px solid ${INK}`, borderRadius: 2, background: INK, color: PAPER }}>
          <div style={{ fontFamily: '"Fraunces", serif', fontSize: 16,
            fontStyle: 'italic', marginBottom: 4 }}>
            + eigenes
          </div>
          <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
            letterSpacing: '0.1em', color: LIGHT, textTransform: 'uppercase' }}>
            template anlegen
          </div>
        </button>
      </div>

      <p style={{ fontFamily: '"Fraunces", serif', fontSize: 12, color: DARK,
        fontStyle: 'italic', marginTop: 'auto', textAlign: 'center',
        opacity: 0.65, lineHeight: 1.6 }}>
        templates kannst du später aus jeder notiz speichern.
      </p>
    </div>
  );
}

// ============================================================================
// NOTE EDITOR — lesen (rendered) ↔ schreiben (raw markdown + toolbar)
// ============================================================================
function NoteEditorScreen({ state, noteId, onBack, onUpdate, onTogglePin, onCycleVisibility, onDelete, onHandoff }) {
  const n = state.notes.find(x => x.id === noteId);
  const [mode, setMode] = useState(n && !n.body ? 'write' : 'read');
  const [draft, setDraft] = useState(n ? { title: n.title, body: n.body } : { title: '', body: '' });
  const [confirmDel, setConfirmDel] = useState(false);
  const [folderPickOpen, setFolderPickOpen] = useState(false);

  if (!n) return null;

  const commit = () => onUpdate(noteId, draft);
  const visLabel = { private: 'privat', friends: 'freund_innen', public: 'öffentlich' }[n.visibility];

  const insert = (before, after = '') => {
    const ta = document.getElementById('note-body-ta');
    if (!ta) { setDraft(d => ({ ...d, body: d.body + before + after })); return; }
    const start = ta.selectionStart;
    const end = ta.selectionEnd;
    const current = draft.body;
    const middle = current.slice(start, end);
    const next = current.slice(0, start) + before + middle + after + current.slice(end);
    setDraft(d => ({ ...d, body: next }));
    setTimeout(() => {
      ta.focus();
      const pos = start + before.length + middle.length;
      ta.setSelectionRange(pos, pos);
    }, 0);
  };

  return (
    <div className="flex-1 flex flex-col relative" style={{ background: PAPER }}>
      {/* Header */}
      <div className="flex items-center justify-between px-5 py-2.5"
        style={{ borderBottom: `1px dashed ${LIGHT}` }}>
        <button onClick={() => { commit(); onBack(); }}
          style={{ color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          ← zurück
        </button>
        <div className="flex items-center gap-1.5">
          <button onClick={() => onTogglePin(noteId)}
            style={{ padding: '4px 7px', fontSize: 11,
              color: n.pinned ? INK : MID }}>★</button>
          <button onClick={() => onCycleVisibility(noteId)}
            style={{ padding: '4px 8px', border: `1px solid ${LIGHT}`, borderRadius: 2,
              fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
              letterSpacing: '0.15em', color: DARK, textTransform: 'uppercase' }}>
            {visLabel}
          </button>
          <button onClick={() => { commit(); setMode(mode === 'read' ? 'write' : 'read'); }}
            style={{ padding: '4px 8px', border: `1px solid ${INK}`, borderRadius: 2,
              background: INK, color: PAPER,
              fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
              letterSpacing: '0.15em', textTransform: 'uppercase' }}>
            {mode === 'read' ? 'schreiben' : 'lesen'}
          </button>
        </div>
      </div>

      {/* Title field */}
      <input
        type="text"
        value={draft.title}
        onChange={(e) => setDraft(d => ({ ...d, title: e.target.value }))}
        placeholder="titel"
        className="px-5 py-3 bg-transparent"
        style={{ fontFamily: '"Fraunces", serif', fontSize: 18, fontStyle: 'italic',
          color: INK, outline: 'none', borderBottom: `1px dashed ${LIGHT}` }}
      />

      {/* Folder + meta strip */}
      <div className="flex items-center justify-between px-5 py-2"
        style={{ borderBottom: `1px dotted ${LIGHT}`,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
        <button onClick={() => setFolderPickOpen(!folderPickOpen)}
          style={{ color: DARK }}>
          ordner · {n.folder || '—'} ▾
        </button>
        <div className="flex items-center gap-3">
          <button onClick={() => { commit(); onHandoff(n); }}
            className="flex items-center gap-1"
            style={{ color: DARK }}>
            {I.share} übergeben
          </button>
          <button onClick={() => setConfirmDel(true)} style={{ color: MID }}>
            löschen
          </button>
        </div>
      </div>

      {/* Body */}
      <div className="flex-1 overflow-y-auto px-5 py-3">
        {mode === 'read' ? (
          draft.body ? <Markdown text={draft.body} /> : (
            <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: MID,
              fontStyle: 'italic', opacity: 0.6 }}>
              noch leer. „schreiben" tippen, um zu beginnen.
            </p>
          )
        ) : (
          <textarea
            id="note-body-ta"
            value={draft.body}
            onChange={(e) => setDraft(d => ({ ...d, body: e.target.value }))}
            placeholder="was denkst du?"
            className="w-full h-full bg-transparent resize-none"
            style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 13,
              color: INK, outline: 'none', lineHeight: 1.6, minHeight: 280 }}
          />
        )}
      </div>

      {/* Format toolbar */}
      {mode === 'write' && (
        <div className="flex gap-1 px-3 py-1.5"
          style={{ borderTop: `1px solid ${LIGHT}`, background: '#d8d1be' }}>
          <FmtBtn label="h1" onClick={() => insert('# ')} />
          <FmtBtn label="h2" onClick={() => insert('## ')} />
          <FmtBtn label="•"  onClick={() => insert('- ')} />
          <FmtBtn label="☐"  onClick={() => insert('- [ ] ')} />
          <FmtBtn label="B"  bold onClick={() => insert('**', '**')} />
          <FmtBtn label="I"  italic onClick={() => insert('*', '*')} />
          <FmtBtn label="❝"  onClick={() => insert('> ')} />
          <FmtBtn label="—"  onClick={() => insert('\n---\n')} />
        </div>
      )}

      {/* Folder picker popover */}
      {folderPickOpen && (
        <div className="absolute left-4 right-4" style={{ top: 120,
          background: PAPER, border: `1px solid ${INK}`, borderRadius: 2,
          boxShadow: '0 4px 16px rgba(0,0,0,0.15)', padding: 10, zIndex: 30 }}>
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.2em', color: MID, textTransform: 'uppercase', marginBottom: 8 }}>
            ordner wählen
          </p>
          <div className="flex flex-wrap gap-1.5">
            {state.note_folders.map(f => (
              <button key={f}
                onClick={() => { onUpdate(noteId, { folder: f }); setFolderPickOpen(false); }}
                style={{ padding: '5px 9px', borderRadius: 2,
                  border: `1px ${n.folder === f ? 'solid' : 'dashed'} ${n.folder === f ? INK : LIGHT}`,
                  background: n.folder === f ? INK : 'transparent',
                  color: n.folder === f ? PAPER : DARK,
                  fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                  letterSpacing: '0.15em', textTransform: 'uppercase' }}>
                {f}
              </button>
            ))}
          </div>
        </div>
      )}

      {/* Delete confirm dialog */}
      {confirmDel && (
        <div className="absolute inset-0 flex items-center justify-center p-6"
          style={{ background: 'rgba(20,16,12,0.4)', zIndex: 40 }}>
          <div style={{ background: PAPER, border: `1.5px solid ${INK}`,
            padding: 18, borderRadius: 2, width: '100%' }}>
            <p style={{ fontFamily: '"Fraunces", serif', fontSize: 16,
              fontStyle: 'italic', color: INK, marginBottom: 6 }}>
              notiz löschen?
            </p>
            <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
              opacity: 0.75, marginBottom: 16 }}>
              „{n.title || '— ohne titel —'}" verschwindet endgültig.
            </p>
            <div className="flex gap-2">
              <button onClick={() => setConfirmDel(false)}
                style={{ flex: 1, padding: '8px', border: `1px solid ${MID}`, color: DARK,
                  fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
                  letterSpacing: '0.2em', textTransform: 'uppercase' }}>
                abbrechen
              </button>
              <button onClick={() => { setConfirmDel(false); onDelete(noteId); onBack(); }}
                style={{ flex: 1, padding: '8px', background: INK, color: PAPER,
                  fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
                  letterSpacing: '0.2em', textTransform: 'uppercase' }}>
                löschen
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function FmtBtn({ label, onClick, bold, italic }) {
  return (
    <button onClick={onClick}
      style={{ flex: 1, padding: '6px 0', background: PAPER,
        border: `1px solid ${MID}`, borderRadius: 2,
        fontFamily: bold || italic ? '"Fraunces", serif' : '"JetBrains Mono", monospace',
        fontWeight: bold ? 700 : 400, fontStyle: italic ? 'italic' : 'normal',
        fontSize: 11, color: DARK }}>
      {label}
    </button>
  );
}

// ============================================================================
// READER · FEED · NEARBY · MOOD — legacy standalone versions still available
// ============================================================================
function ReaderScreen({ state }) {
  const r = state.reader;
  return (
    <div className="flex-1 flex flex-col px-7 py-5" style={{ background: PAPER }}>
      <div style={{ textAlign: 'center' }}>
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>{r.author}</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, fontStyle: 'italic',
          color: INK, marginTop: 2 }}>{r.book}</h2>
      </div>
      <div className="flex-1 flex items-center justify-center mt-4 mb-2">
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14.5, lineHeight: 1.65,
          color: DARK, fontWeight: 400, textAlign: 'justify', hyphens: 'auto', maxWidth: 340 }}>
          {r.excerpt}
        </p>
      </div>
      <div className="flex items-center justify-between pt-3"
        style={{ borderTop: `1px dotted ${LIGHT}`, fontFamily: '"JetBrains Mono", monospace',
          fontSize: 10, letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>
        <span>← zurück</span>
        <span>{r.page} / {r.total}</span>
        <span>weiter →</span>
      </div>
    </div>
  );
}

function FeedScreen({ state }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-5" style={{ background: PAPER }}>
      <Title kicker="feed" title="was liest die welt." />
      <div className="flex-1 flex flex-col mt-4">
        {state.feed.map(item => (
          <div key={item.id} className="py-3" style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            <div className="flex items-center justify-between mb-1">
              <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>{item.src}</span>
              <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.1em', color: MID }}>{item.date}</span>
            </div>
            <div className="flex items-start gap-2">
              {item.unread && <span style={{ color: INK, fontSize: 16, lineHeight: 1 }}>•</span>}
              <p style={{ fontFamily: '"Fraunces", serif', fontSize: 15, color: INK,
                fontStyle: item.unread ? 'normal' : 'italic', opacity: item.unread ? 1 : 0.55,
                fontWeight: item.unread ? 500 : 400 }}>{item.title}</p>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

function NearbyScreen({ state, go }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <Title kicker="in der nähe" title="freund_innen, lose." />
      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.18em', color: MID, marginTop: 4, textTransform: 'uppercase' }}>
        via {state.sync.roomServer} · {state.nearby.length} gefunden
      </p>

      <div className="flex-1 flex flex-col gap-2 mt-4">
        {state.nearby.map(n => {
          const mood = MOODS.find(m => m.id === n.mood);
          return (
            <button key={n.id} onClick={() => go(S.FRIEND, n.id)}
              className="flex items-start gap-3 py-2.5 text-left"
              style={{ borderBottom: `1px dotted ${LIGHT}` }}>
              <div style={{ width: 36, height: 36, borderRadius: '50%',
                border: `1.5px solid ${INK}`, display: 'flex', alignItems: 'center',
                justifyContent: 'center', fontFamily: '"Fraunces", serif',
                fontSize: 16, color: INK, fontStyle: 'italic', flexShrink: 0 }}>
                {n.name[0]}
              </div>
              <div className="flex-1">
                <div className="flex items-center justify-between">
                  <span style={{ fontFamily: '"Fraunces", serif', fontSize: 16, color: INK, fontWeight: 500 }}>
                    {n.name}
                  </span>
                  <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                    letterSpacing: '0.15em', color: MID }}>{n.dist}</span>
                </div>
                {mood && (
                  <div className="flex items-center gap-1.5 mt-0.5">
                    <span style={{ color: DARK, fontSize: 11 }}>{mood.icon}</span>
                    <span style={{ fontFamily: '"Fraunces", serif', fontSize: 13,
                      color: DARK, fontStyle: 'italic' }}>
                      mag gerade {mood.label}
                    </span>
                  </div>
                )}
                <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                  letterSpacing: '0.15em', color: MID, marginTop: 3, textTransform: 'uppercase' }}>
                  {n.last_heard}
                </div>
              </div>
            </button>
          );
        })}
      </div>

      <button onClick={() => go(S.MOOD)}
        style={{ marginTop: 8, padding: '11px', background: INK, color: PAPER,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        selbst etwas teilen
      </button>
    </div>
  );
}

// ============================================================================
// CHATS — list of conversations
// ============================================================================
function ChatsScreen({ state, onOpenChat }) {
  const kindLabel = { direct: 'direkt', group: 'gruppe', public: 'öffentlich' };
  const resetLabel = (r) =>
    r === 'daily' ? 'reset · täglich' : r === 'weekly' ? 'reset · wöchentlich' : null;

  return (
    <div className="flex-1 flex flex-col px-7 py-5" style={{ background: PAPER }}>
      <Title kicker="gespräche" title="reden, leise." />

      <div className="flex-1 flex flex-col mt-4">
        {state.chats.map(c => (
          <button key={c.id} onClick={() => onOpenChat(c.id)}
            className="flex items-start gap-3 py-3 text-left"
            style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            {/* avatar */}
            <div style={{ width: 34, height: 34, flexShrink: 0,
              border: `1.4px ${c.kind === 'public' ? 'dashed' : 'solid'} ${INK}`,
              borderRadius: c.kind === 'direct' ? '50%' : 3,
              background: c.kind === 'public' ? 'transparent' : PAPER,
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              fontFamily: '"Fraunces", serif', fontSize: 14, color: INK,
              fontStyle: 'italic' }}>
              {c.kind === 'public' ? '#' : c.name[0]}
            </div>
            {/* body */}
            <div className="flex-1 min-w-0">
              <div className="flex items-center justify-between gap-2">
                <span style={{ fontFamily: '"Fraunces", serif', fontSize: 15,
                  color: INK, fontWeight: 500,
                  whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                  {c.name}
                </span>
                <div className="flex items-center gap-1.5 flex-shrink-0">
                  {c.unread > 0 && (
                    <span style={{ background: INK, color: PAPER,
                      fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
                      padding: '2px 5px', borderRadius: 2, letterSpacing: '0.05em' }}>
                      {c.unread}
                    </span>
                  )}
                  <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                    letterSpacing: '0.12em', color: MID, textTransform: 'uppercase' }}>
                    {c.ts}
                  </span>
                </div>
              </div>
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
                opacity: 0.75, marginTop: 1,
                whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                {c.last}
              </div>
              <div className="flex items-center gap-2 mt-1"
                style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
                  letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
                <span>{kindLabel[c.kind]}</span>
                {resetLabel(c.reset) && <span>· {resetLabel(c.reset)}</span>}
              </div>
            </div>
          </button>
        ))}
      </div>

      <button style={{ marginTop: 6, padding: '10px', border: `1px dashed ${MID}`,
        color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.25em', textTransform: 'uppercase',
        display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}>
        {I.plus} neues gespräch
      </button>
    </div>
  );
}

// ============================================================================
// CHAT — single conversation view
// ============================================================================
function ChatScreen({ state, chatId, onBack, onSend }) {
  const c = state.chats.find(x => x.id === chatId);
  if (!c) return null;
  const resetIn = c.reset === 'daily' ? 'noch 14h · dann leer'
                : c.reset === 'weekly' ? 'noch 3 tage · dann leer'
                : null;

  return (
    <div className="flex-1 flex flex-col" style={{ background: PAPER }}>
      {/* header */}
      <div className="flex items-center gap-3 px-5 py-3"
        style={{ borderBottom: `1px dashed ${LIGHT}` }}>
        <button onClick={onBack}
          style={{ color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 14 }}>
          ←
        </button>
        <div className="flex-1">
          <div style={{ fontFamily: '"Fraunces", serif', fontSize: 15, color: INK, fontWeight: 500 }}>
            {c.name}
          </div>
          {resetIn && (
            <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
              letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
              {resetIn}
            </div>
          )}
        </div>
      </div>

      {/* messages */}
      <div className="flex-1 px-5 py-4 overflow-y-auto flex flex-col gap-3">
        {c.messages.length === 0 ? (
          <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: MID,
            fontStyle: 'italic', textAlign: 'center', marginTop: 40, opacity: 0.7 }}>
            noch keine nachrichten.<br/>
            {c.reset === 'weekly' ? 'wird wöchentlich geleert.' :
             c.reset === 'daily'  ? 'wird täglich geleert.' :
             'hier schreibt sich\'s entspannt.'}
          </p>
        ) : c.messages.map((msg, i) => {
          const isMine = msg.from === 'levin' || msg.from === 'me';
          return (
            <div key={i} className={`flex flex-col ${isMine ? 'items-end' : 'items-start'}`}>
              {!isMine && (
                <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
                  letterSpacing: '0.15em', color: MID, marginBottom: 2, textTransform: 'uppercase' }}>
                  {msg.from}
                </div>
              )}
              <div style={{
                background: isMine ? INK : 'transparent',
                color: isMine ? PAPER : INK,
                border: isMine ? 'none' : `1px solid ${MID}`,
                padding: '7px 11px', borderRadius: 2, maxWidth: '80%',
                fontFamily: '"Fraunces", serif', fontSize: 13.5, lineHeight: 1.45,
              }}>
                {msg.text}
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8,
                letterSpacing: '0.1em', color: MID, marginTop: 2 }}>
                {msg.ts}
              </div>
            </div>
          );
        })}
      </div>

      {/* compose bar */}
      <button onClick={() => onSend(c.id)}
        className="flex items-center justify-between mx-3 mb-3 px-3 py-3"
        style={{ border: `1px solid ${MID}`, borderRadius: 2,
          fontFamily: '"Fraunces", serif', fontSize: 13, color: MID,
          fontStyle: 'italic' }}>
        <span>nachricht schreiben …</span>
        <span style={{ color: DARK }}>{I.send}</span>
      </button>
    </div>
  );
}

// ============================================================================
// FRIEND PROFILE — what others have made public about themselves
// ============================================================================
function FriendProfileScreen({ state, friendId, onBack, onOpenDM }) {
  const f = state.nearby.find(x => x.id === friendId);
  if (!f) return null;
  const mood = MOODS.find(m => m.id === f.mood);

  return (
    <div className="flex-1 flex flex-col px-7 py-4 overflow-y-auto" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />

      <div className="flex flex-col items-center mt-2">
        <Moki size={100} variant={f.variant} worn={f.worn} />
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontWeight: 500, marginTop: 6 }}>{f.name}</h2>
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>{f.pub_key}</p>
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: DARK,
          fontStyle: 'italic', marginTop: 8, textAlign: 'center', lineHeight: 1.5, maxWidth: 260 }}>
          „{f.bio}"
        </p>
      </div>

      {/* last heard + distance */}
      <div className="flex items-center justify-between mt-5 py-3"
        style={{ borderTop: `1px dotted ${LIGHT}`, borderBottom: `1px dotted ${LIGHT}` }}>
        <div>
          <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
            letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>zuletzt gehört</div>
          <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: INK,
            fontStyle: 'italic', marginTop: 2 }}>{f.last_heard}</div>
        </div>
        <div style={{ textAlign: 'right' }}>
          <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
            letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>entfernung</div>
          <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: INK,
            fontStyle: 'italic', marginTop: 2 }}>{f.dist}</div>
        </div>
      </div>

      {/* active mood */}
      {mood && (
        <div className="mt-4">
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 6 }}>
            gerade
          </p>
          <div style={{ border: `1px solid ${INK}`, padding: '10px 12px', borderRadius: 2,
            display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontSize: 16, color: INK }}>{mood.icon}</span>
            <div>
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: INK, fontWeight: 500 }}>
                {mood.label}
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                letterSpacing: '0.15em', color: MID, marginTop: 1, textTransform: 'uppercase' }}>
                {mood.hint}
              </div>
            </div>
          </div>
        </div>
      )}

      {/* public habits — mini git grids */}
      {f.public_habits && f.public_habits.length > 0 && (
        <div className="mt-4">
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 6 }}>
            öffentliche gewohnheiten
          </p>
          {f.public_habits.map((h, i) => (
            <div key={i} className="py-2"
              style={{ borderBottom: `1px dotted ${LIGHT}` }}>
              <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13,
                color: INK, marginBottom: 4 }}>{h.name}</div>
              <MiniGrid history={h.history} />
            </div>
          ))}
        </div>
      )}

      {/* public books */}
      {f.public_books && f.public_books.length > 0 && (
        <div className="mt-4">
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 6 }}>
            gelesen · öffentlich
          </p>
          <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
            fontStyle: 'italic', lineHeight: 1.7 }}>
            {f.public_books.map((b, i) => <div key={i}>· {b}</div>)}
          </div>
        </div>
      )}

      <button onClick={() => onOpenDM(f.id)}
        style={{ marginTop: 14, padding: '11px', background: INK, color: PAPER,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        nachricht schreiben
      </button>
    </div>
  );
}

// Mini grid — compact version of the habit heatmap for friend profiles
function MiniGrid({ history }) {
  const WEEKS = 12;
  const DAYS = 7;
  return (
    <div style={{ display: 'grid', gridTemplateColumns: `repeat(${WEEKS}, 1fr)`, gap: 1.5 }}>
      {Array.from({ length: WEEKS }).map((_, w) => (
        <div key={w} style={{ display: 'flex', flexDirection: 'column', gap: 1.5 }}>
          {Array.from({ length: DAYS }).map((_, d) => {
            const count = history[w * DAYS + d] || 0;
            return <div key={d} style={{ aspectRatio: '1/1', ...cellStyle(count), borderRadius: 1 }} />;
          })}
        </div>
      ))}
    </div>
  );
}

// ============================================================================
// MAP — tabs: Karte | Nah
// ============================================================================
function MapTabsScreen({ state, tab, setTab, go, onCycleShare, onHandoffHere }) {
  return (
    <div className="flex-1 flex flex-col px-5 py-4" style={{ background: PAPER }}>
      <div className="flex" style={{ border: `1px solid ${INK}`, borderRadius: 2, padding: 2, marginBottom: 10 }}>
        <TabBtn label="karte" active={tab === 'map'}    onClick={() => setTab('map')}    />
        <TabBtn label="in der nähe" active={tab === 'nearby'} onClick={() => setTab('nearby')} />
      </div>
      {tab === 'map'    && <MapContent state={state} onCycleShare={onCycleShare} onHandoffHere={onHandoffHere} />}
      {tab === 'nearby' && <NearbyContent state={state} go={go} />}
    </div>
  );
}

function MapContent({ state, onCycleShare, onHandoffHere }) {
  const m = state.map;
  const shareLabel = { off: 'unsichtbar', hourly: 'stündlich', live: 'live' }[m.share];

  return (
    <div className="flex-1 flex flex-col">
      <div className="flex items-center justify-between px-2 mb-2">
        <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>
          heidelberg · ~1km
        </span>
        <button onClick={onCycleShare}
          style={{ border: `1px solid ${m.share === 'live' ? INK : LIGHT}`,
            background: m.share === 'live' ? INK : 'transparent',
            color: m.share === 'live' ? PAPER : DARK,
            padding: '5px 9px', borderRadius: 2,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          ich · {shareLabel}
        </button>
      </div>

      <div className="relative flex-1" style={{
        border: `1px solid ${MID}`, borderRadius: 2, overflow: 'hidden',
        background: `repeating-linear-gradient(0deg,
          transparent 0 14px, rgba(0,0,0,0.03) 14px 15px),
          repeating-linear-gradient(90deg,
          transparent 0 14px, rgba(0,0,0,0.03) 14px 15px)`,
      }}>
        <svg viewBox="0 0 100 100" preserveAspectRatio="none"
          style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
          <path d="M -5 72 Q 20 68 40 75 Q 60 82 80 74 Q 95 70 105 76"
            stroke={MID} strokeWidth="1.2" fill="none" opacity="0.7" />
          <path d="M -5 74.5 Q 20 70.5 40 77.5 Q 60 84.5 80 76.5 Q 95 72.5 105 78.5"
            stroke={MID} strokeWidth="0.6" fill="none" opacity="0.4" />
          <path d="M 8 20 Q 35 30 55 50 Q 75 70 95 88"
            stroke={DARK} strokeWidth="0.6" fill="none" opacity="0.5" strokeDasharray="0.5 1.5" />
          <path d="M 20 90 Q 40 55 60 45 Q 80 35 92 22"
            stroke={DARK} strokeWidth="0.4" fill="none" opacity="0.35" strokeDasharray="0.3 1" />
          <path d="M 15 25 L 35 20 L 40 38 L 20 44 Z" fill={MID} opacity="0.12" />
          <path d="M 15 25 L 35 20 L 40 38 L 20 44 Z"
            stroke={MID} strokeWidth="0.4" fill="none" opacity="0.35" strokeDasharray="0.4 0.8" />
        </svg>

        {m.places.map(p => <Pin key={p.id} x={p.x} y={p.y} kind="place" label={p.name} />)}
        {m.events.map(ev => <Pin key={'e'+ev.id} x={ev.x} y={ev.y} kind="event" label={`${ev.hour} · ${ev.title}`} />)}
        {m.friends.map(f => <Pin key={f.id} x={f.x} y={f.y} kind="friend" label={`${f.name} · ${f.fresh}`} />)}
        <SelfPin x={m.self.x} y={m.self.y} pinging={m.share === 'live'} />
      </div>

      <div className="flex items-center justify-between mt-2 px-1"
        style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
          letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
        <LegendDot shape="self"   label="ich" />
        <LegendDot shape="friend" label={`${m.friends.length} live`} />
        <LegendDot shape="place"  label={`${m.places.length} orte`} />
        <LegendDot shape="event"  label={`${m.events.length} heute`} />
      </div>

      <button onClick={() => onHandoffHere && onHandoffHere({ name: 'hier', x: m.self.x, y: m.self.y })}
        className="flex items-center justify-center gap-2 mt-3 py-2.5"
        style={{ border: `1px dashed ${DARK}`, color: DARK,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        {I.share} diesen ort übergeben
      </button>
    </div>
  );
}

function NearbyContent({ state, go }) {
  return (
    <div className="flex-1 flex flex-col">
      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.18em', color: MID, textTransform: 'uppercase', marginBottom: 8 }}>
        via {state.sync.roomServer} · {state.nearby.length} gefunden
      </p>
      <div className="flex-1 flex flex-col gap-2">
        {state.nearby.map(n => {
          const mood = MOODS.find(m => m.id === n.mood);
          return (
            <button key={n.id} onClick={() => go(S.FRIEND, n.id)}
              className="flex items-start gap-3 py-2.5 text-left"
              style={{ borderBottom: `1px dotted ${LIGHT}` }}>
              <div style={{ width: 36, height: 36, borderRadius: '50%',
                border: `1.5px solid ${INK}`, display: 'flex', alignItems: 'center',
                justifyContent: 'center', fontFamily: '"Fraunces", serif',
                fontSize: 16, color: INK, fontStyle: 'italic', flexShrink: 0 }}>
                {n.name[0]}
              </div>
              <div className="flex-1">
                <div className="flex items-center justify-between">
                  <span style={{ fontFamily: '"Fraunces", serif', fontSize: 16, color: INK, fontWeight: 500 }}>
                    {n.name}
                  </span>
                  <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                    letterSpacing: '0.15em', color: MID }}>{n.dist}</span>
                </div>
                {mood && (
                  <div className="flex items-center gap-1.5 mt-0.5">
                    <span style={{ color: DARK, fontSize: 11 }}>{mood.icon}</span>
                    <span style={{ fontFamily: '"Fraunces", serif', fontSize: 13,
                      color: DARK, fontStyle: 'italic' }}>mag gerade {mood.label}</span>
                  </div>
                )}
                <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                  letterSpacing: '0.15em', color: MID, marginTop: 3, textTransform: 'uppercase' }}>
                  {n.last_heard}
                </div>
              </div>
            </button>
          );
        })}
      </div>
      <button onClick={() => go(S.MOOD)}
        style={{ marginTop: 8, padding: '11px', background: INK, color: PAPER,
          fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', textTransform: 'uppercase' }}>
        selbst etwas teilen
      </button>
    </div>
  );
}

// ============================================================================
// LEGACY MAP — standalone map screen kept for direct reference / deep links
// ============================================================================
function MapScreen({ state, onCycleShare }) {
  const m = state.map;
  const shareLabel = { off: 'unsichtbar', hourly: 'stündlich', live: 'live' }[m.share];

  return (
    <div className="flex-1 flex flex-col px-5 py-4" style={{ background: PAPER }}>
      <div className="flex items-start justify-between px-2">
        <div>
          <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>karte</p>
          <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
            fontStyle: 'italic', marginTop: 2 }}>hier und jetzt.</h2>
        </div>
        <button onClick={onCycleShare}
          style={{ border: `1px solid ${m.share === 'live' ? INK : LIGHT}`,
            background: m.share === 'live' ? INK : 'transparent',
            color: m.share === 'live' ? PAPER : DARK,
            padding: '6px 10px', borderRadius: 2,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          ich · {shareLabel}
        </button>
      </div>

      {/* THE MAP — stylized vector drawing */}
      <div className="relative flex-1 mt-3" style={{
        border: `1px solid ${MID}`, borderRadius: 2, overflow: 'hidden',
        background: `repeating-linear-gradient(0deg,
          transparent 0 14px, rgba(0,0,0,0.03) 14px 15px),
          repeating-linear-gradient(90deg,
          transparent 0 14px, rgba(0,0,0,0.03) 14px 15px)`,
      }}>
        <svg viewBox="0 0 100 100" preserveAspectRatio="none"
          style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
          {/* River-like curve — the Neckar, spiritually */}
          <path d="M -5 72 Q 20 68 40 75 Q 60 82 80 74 Q 95 70 105 76"
            stroke={MID} strokeWidth="1.2" fill="none" opacity="0.7" />
          <path d="M -5 74.5 Q 20 70.5 40 77.5 Q 60 84.5 80 76.5 Q 95 72.5 105 78.5"
            stroke={MID} strokeWidth="0.6" fill="none" opacity="0.4" />

          {/* Main road */}
          <path d="M 8 20 Q 35 30 55 50 Q 75 70 95 88"
            stroke={DARK} strokeWidth="0.6" fill="none" opacity="0.5" strokeDasharray="0.5 1.5" />
          {/* Secondary road */}
          <path d="M 20 90 Q 40 55 60 45 Q 80 35 92 22"
            stroke={DARK} strokeWidth="0.4" fill="none" opacity="0.35" strokeDasharray="0.3 1" />

          {/* Park patch */}
          <path d="M 15 25 L 35 20 L 40 38 L 20 44 Z"
            fill={MID} opacity="0.12" />
          <path d="M 15 25 L 35 20 L 40 38 L 20 44 Z"
            stroke={MID} strokeWidth="0.4" fill="none" opacity="0.35" strokeDasharray="0.4 0.8" />
        </svg>

        {/* Places (saved) */}
        {m.places.map(p => (
          <Pin key={p.id} x={p.x} y={p.y} kind="place" label={p.name} />
        ))}
        {/* Events today */}
        {m.events.map(ev => (
          <Pin key={'e'+ev.id} x={ev.x} y={ev.y} kind="event"
            label={`${ev.hour} · ${ev.title}`} />
        ))}
        {/* Friends live */}
        {m.friends.map(f => (
          <Pin key={f.id} x={f.x} y={f.y} kind="friend"
            label={`${f.name} · ${f.fresh}`} />
        ))}
        {/* Self — always last, on top, with ping ring */}
        <SelfPin x={m.self.x} y={m.self.y} pinging={m.share === 'live'} />
      </div>

      {/* Legend / key */}
      <div className="flex items-center justify-between mt-3 px-1"
        style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
          letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
        <LegendDot shape="self"   label="ich" />
        <LegendDot shape="friend" label={`${m.friends.length} live`} />
        <LegendDot shape="place"  label={`${m.places.length} orte`} />
        <LegendDot shape="event"  label={`${m.events.length} heute`} />
      </div>
    </div>
  );
}

function Pin({ x, y, kind, label }) {
  // On real device: these would be static bitmap icons.
  // In simulator: tooltip on tap would be nice but we just always show small label.
  const size = kind === 'event' ? 10 : 8;
  const showLabel = kind === 'friend' || kind === 'event';
  return (
    <div style={{ position: 'absolute', left: `${x}%`, top: `${y}%`,
      transform: 'translate(-50%, -50%)', textAlign: 'center' }}>
      {kind === 'place' && (
        <div style={{ width: size, height: size, border: `1.2px solid ${INK}`,
          background: PAPER, borderRadius: 1 }} />
      )}
      {kind === 'friend' && (
        <div style={{ width: size+2, height: size+2, border: `1.5px solid ${INK}`,
          background: INK, borderRadius: '50%' }} />
      )}
      {kind === 'event' && (
        <div style={{ fontSize: size+3, color: INK, lineHeight: 1 }}>◆</div>
      )}
      {showLabel && (
        <div style={{ fontFamily: '"Fraunces", serif', fontSize: 9.5,
          fontStyle: 'italic', color: INK, marginTop: 2, whiteSpace: 'nowrap',
          background: PAPER, padding: '0 2px' }}>
          {label}
        </div>
      )}
      {!showLabel && (
        <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 7.5,
          letterSpacing: '0.1em', color: MID, marginTop: 1, whiteSpace: 'nowrap',
          textTransform: 'uppercase' }}>
          {label}
        </div>
      )}
    </div>
  );
}

function SelfPin({ x, y, pinging }) {
  return (
    <div style={{ position: 'absolute', left: `${x}%`, top: `${y}%`,
      transform: 'translate(-50%, -50%)' }}>
      {pinging && (
        <div style={{ position: 'absolute', inset: -10,
          border: `1.5px solid ${INK}`, borderRadius: '50%',
          animation: 'ping-ring 2.2s ease-out infinite' }} />
      )}
      <div style={{ width: 14, height: 14, background: INK,
        border: `2.5px solid ${PAPER}`, borderRadius: '50%',
        boxShadow: `0 0 0 1.5px ${INK}` }} />
    </div>
  );
}

function LegendDot({ shape, label }) {
  return (
    <span className="flex items-center gap-1">
      {shape === 'self'   && <span style={{ width: 6, height: 6, background: INK, borderRadius: '50%' }} />}
      {shape === 'friend' && <span style={{ width: 6, height: 6, background: INK, borderRadius: '50%', opacity: 0.7 }} />}
      {shape === 'place'  && <span style={{ width: 6, height: 6, border: `1px solid ${INK}`, background: PAPER }} />}
      {shape === 'event'  && <span style={{ color: INK, fontSize: 10, lineHeight: 0.6 }}>◆</span>}
      <span>{label}</span>
    </span>
  );
}

function MoodScreen({ state, onSetMood, onClearMood, onBack }) {
  const active = state.activeMood;
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />
      <div className="mt-1">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>teilen</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontStyle: 'italic', marginTop: 2 }}>wonach ist dir?</h2>
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
          fontStyle: 'italic', opacity: 0.75, marginTop: 6, lineHeight: 1.5 }}>
          wird im nächsten sync im freundeskreis sichtbar.<br/>
          gilt bis mitternacht.
        </p>
      </div>

      <div className="grid grid-cols-2 gap-2 mt-5">
        {MOODS.map(m => {
          const isActive = active && active.id === m.id;
          return (
            <button key={m.id} onClick={() => onSetMood(m.id)}
              style={{ padding: '11px 10px', textAlign: 'left',
                border: `${isActive ? 1.5 : 1}px ${isActive ? 'solid' : 'dashed'} ${isActive ? INK : LIGHT}`,
                background: isActive ? INK : 'transparent',
                color: isActive ? PAPER : INK, borderRadius: 2 }}>
              <div className="flex items-center gap-2">
                <span style={{ fontSize: 16 }}>{m.icon}</span>
                <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, fontWeight: 500 }}>
                  {m.label}
                </span>
              </div>
              <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
                letterSpacing: '0.1em', color: isActive ? LIGHT : MID, marginTop: 3,
                textTransform: 'uppercase' }}>{m.hint}</div>
            </button>
          );
        })}
      </div>

      {active && (
        <button onClick={onClearMood}
          style={{ marginTop: 10, padding: '8px', border: `1px dashed ${MID}`,
            color: MID, fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          nichts mehr teilen
        </button>
      )}
    </div>
  );
}

// ============================================================================
// HANDOFF SHEET — BLE proximity pairing ritual
// Hardware: both devices run BLE scanner when active. RSSI > -60 dBm means
// very close (~20cm). Only then is the handoff prompt shown. Payload is JSON
// over a short-lived GATT connection, typically <2KB.
// ============================================================================
function HandoffSheet({ handoff, setHandoff, onConfirm }) {
  const [phase, setPhase] = useState('searching'); // searching | found | sending | done
  const [foundDevice, setFoundDevice] = useState(null);

  useEffect(() => {
    if (!handoff) return;
    setPhase('searching');
    setFoundDevice(null);
    // Simulate BLE discovery: after ~1.8s, pretend we found lina's Moki
    const t1 = setTimeout(() => {
      setFoundDevice({ name: 'lina', key: 'HDB-7f2a', rssi: -52 });
      setPhase('found');
    }, 1800);
    return () => clearTimeout(t1);
  }, [handoff]);

  if (!handoff) return null;

  const sendNow = () => {
    setPhase('sending');
    setTimeout(() => {
      setPhase('done');
      setTimeout(() => {
        onConfirm(foundDevice);
        setHandoff(null);
      }, 900);
    }, 1200);
  };

  const kindLabel = {
    note: 'notiz', book: 'buch', place: 'ort',
    recipe: 'rezept', habit: 'gewohnheit', event: 'termin',
  }[handoff.kind] || 'sache';

  return (
    <div className="absolute inset-0 flex flex-col items-center justify-center"
      style={{ background: PAPER, zIndex: 25, padding: 24 }}>
      <button onClick={() => setHandoff(null)}
        className="absolute top-4 left-4"
        style={{ color: MID, fontFamily: '"JetBrains Mono", monospace',
          fontSize: 10, letterSpacing: '0.2em', textTransform: 'uppercase' }}>
        abbrechen
      </button>

      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>
        übergeben · bluetooth
      </p>
      <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
        fontStyle: 'italic', marginTop: 4, textAlign: 'center' }}>
        {phase === 'searching' && 'halte dein moki an das andere'}
        {phase === 'found'     && 'gefunden.'}
        {phase === 'sending'   && 'schickt …'}
        {phase === 'done'      && 'übergeben.'}
      </h2>

      {/* The pulsing ring visualization */}
      <div className="relative my-10" style={{ width: 160, height: 160 }}>
        {/* outer rings for searching state */}
        {phase === 'searching' && [0, 0.4, 0.8].map((delay, i) => (
          <div key={i} style={{
            position: 'absolute', inset: 0, border: `1.5px solid ${INK}`,
            borderRadius: '50%', animation: `handoff-ring 2s ease-out ${delay}s infinite`,
          }} />
        ))}
        {/* solid ring for found */}
        {phase === 'found' && (
          <div style={{
            position: 'absolute', inset: 20, border: `2px solid ${INK}`,
            borderRadius: '50%',
          }} />
        )}
        {phase === 'sending' && (
          <div style={{
            position: 'absolute', inset: 20, border: `2px solid ${INK}`,
            borderRadius: '50%', borderRightColor: 'transparent',
            animation: 'spin 0.8s linear infinite',
          }} />
        )}
        {/* center dot */}
        <div style={{
          position: 'absolute', top: '50%', left: '50%',
          transform: 'translate(-50%, -50%)',
          width: 40, height: 40, background: INK, borderRadius: '50%',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          color: PAPER, fontFamily: '"Fraunces", serif', fontSize: 16, fontStyle: 'italic',
        }}>
          {phase === 'done' ? '✓' : 'moki'}
        </div>
      </div>

      {/* Content preview */}
      <div style={{ border: `1px dashed ${MID}`, padding: '10px 14px', borderRadius: 2,
        minWidth: 220, textAlign: 'center' }}>
        <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 8.5,
          letterSpacing: '0.2em', color: MID, textTransform: 'uppercase', marginBottom: 4 }}>
          {kindLabel} · du übergibst
        </div>
        <div style={{ fontFamily: '"Fraunces", serif', fontSize: 14,
          fontStyle: 'italic', color: INK }}>
          {handoff.label}
        </div>
      </div>

      {/* Found device / action */}
      {phase === 'found' && foundDevice && (
        <button onClick={sendNow}
          className="mt-6 px-6 py-3"
          style={{ background: INK, color: PAPER,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.25em', textTransform: 'uppercase' }}>
          an {foundDevice.name} übergeben
        </button>
      )}

      {phase === 'searching' && (
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 12, color: MID,
          fontStyle: 'italic', marginTop: 16, opacity: 0.7, textAlign: 'center' }}>
          suche nach moki in deiner nähe …<br/>ca. 20 cm abstand genügt.
        </p>
      )}

      {phase === 'done' && foundDevice && (
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
          fontStyle: 'italic', marginTop: 16, textAlign: 'center' }}>
          {foundDevice.name} hat „{handoff.label}" erhalten.
        </p>
      )}
    </div>
  );
}

// ============================================================================
// INBOX — things others have handed to you
// ============================================================================
function InboxScreen({ state, onBack, onAccept, onDismiss }) {
  const kindLabel = { note: 'notiz', book: 'buch', place: 'ort', recipe: 'rezept' };
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />
      <div className="mt-1">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>sammelbox</p>
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontStyle: 'italic', marginTop: 2 }}>was andere dir gaben.</h2>
      </div>

      <div className="flex-1 flex flex-col gap-3 mt-5">
        {state.inbox.length === 0 && (
          <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: MID,
            fontStyle: 'italic', textAlign: 'center', marginTop: 40, opacity: 0.65,
            lineHeight: 1.6 }}>
            noch nichts erhalten.<br/>
            <span style={{ fontSize: 11 }}>
              halte dein moki an das eines freundes.
            </span>
          </p>
        )}
        {state.inbox.map(item => (
          <div key={item.id} className="py-3"
            style={{ borderBottom: `1px dotted ${LIGHT}` }}>
            <div className="flex items-center justify-between mb-1"
              style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                letterSpacing: '0.15em', color: MID, textTransform: 'uppercase' }}>
              <span>{kindLabel[item.kind]} · von {item.from}</span>
              <span>{item.at}</span>
            </div>
            <p style={{ fontFamily: '"Fraunces", serif', fontSize: 15, color: INK,
              fontWeight: 500, marginBottom: 8 }}>
              {item.preview}
            </p>
            <div className="flex gap-2">
              <button onClick={() => onAccept(item.id)}
                style={{ flex: 1, padding: '7px', background: INK, color: PAPER,
                  fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                  letterSpacing: '0.2em', textTransform: 'uppercase' }}>
                behalten
              </button>
              <button onClick={() => onDismiss(item.id)}
                style={{ flex: 1, padding: '7px', border: `1px solid ${MID}`, color: DARK,
                  fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                  letterSpacing: '0.2em', textTransform: 'uppercase' }}>
                verwerfen
              </button>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ============================================================================
// PROFILE
// ============================================================================
function ProfileScreen({ state, onBack }) {
  const p = state.profile;
  const totalHabitTaps = state.habits.reduce(
    (s, h) => s + h.history.reduce((x, v) => x + v, 0), 0);
  const visLabel = { friends: 'nur freund_innen', public: 'öffentlich', private: 'privat' }[p.visibility];

  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />
      <div className="flex flex-col items-center mt-2">
        <Moki size={90} mood={state.pet.mood} variant={state.pet.variant} worn={state.pet.worn} />
        <h2 style={{ fontFamily: '"Fraunces", serif', fontSize: 22, color: INK,
          fontWeight: 500, marginTop: 6 }}>{p.handle}</h2>
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>{p.pub_key}</p>
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: DARK,
          fontStyle: 'italic', marginTop: 8, textAlign: 'center', lineHeight: 1.5, maxWidth: 260 }}>
          „{p.bio}"
        </p>
      </div>

      <div className="flex items-center justify-between mt-5 py-3"
        style={{ borderTop: `1px dotted ${LIGHT}`, borderBottom: `1px dotted ${LIGHT}` }}>
        <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.2em', color: MID, textTransform: 'uppercase' }}>sichtbar für</span>
        <span style={{ fontFamily: '"Fraunces", serif', fontSize: 14, color: INK, fontStyle: 'italic' }}>
          {visLabel}
        </span>
      </div>

      <div className="grid grid-cols-3 gap-2 mt-4">
        <Stat label="tage"    value={state.pet.age_days} sub="mit moki" />
        <Stat label="habits"  value={totalHabitTaps}     sub="taps" />
        <Stat label="serie"   value={state.pet.streak}   sub="akt. max" />
      </div>

      <div className="mt-5">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 6 }}>
          öffentlich geteilt
        </p>
        <div style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
          lineHeight: 1.7, fontStyle: 'italic' }}>
          · lesekreis · walden (fr)<br/>
          · mood: {state.activeMood ? MOODS.find(m => m.id === state.activeMood.id)?.label : '—'}
        </div>
      </div>
    </div>
  );
}

// ============================================================================
// MOKI DETAIL — now with wardrobe / accessories
// ============================================================================
function MokiScreen({ state, onSetWorn, onBack }) {
  return (
    <div className="flex-1 flex flex-col px-7 py-4" style={{ background: PAPER }}>
      <BackBar onBack={onBack} label="zurück" />

      <div className="flex flex-col items-center mt-2">
        <Moki size={140} mood={state.pet.mood} variant={state.pet.variant} worn={state.pet.worn} />
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 20, fontWeight: 500, color: INK, marginTop: 8 }}>
          moki
        </p>
        <p style={{ fontFamily: '"Fraunces", serif', fontSize: 13, color: DARK,
          fontStyle: 'italic', marginTop: 2 }}>
          ruhig. zufrieden. atmet.
        </p>
      </div>

      <div className="grid grid-cols-2 gap-2 mt-5">
        <Stat label="stimmung"  value={state.pet.mood}      sub="gerade"  />
        <Stat label="tage"      value={state.pet.age_days}  sub="alt"     />
      </div>

      {/* GARDEROBE */}
      <div className="mt-5">
        <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
          letterSpacing: '0.25em', color: MID, textTransform: 'uppercase', marginBottom: 8 }}>
          garderobe · {state.pet.earned.length} freigeschaltet
        </p>
        <div className="grid grid-cols-3 gap-2">
          <WardrobeSlot label="nichts" active={!state.pet.worn} onClick={() => onSetWorn(null)} />
          {Object.entries(ACCESSORIES).map(([key, acc]) => {
            const earned = state.pet.earned.includes(key);
            const active = state.pet.worn === key;
            return (
              <WardrobeSlot key={key} label={acc.label}
                earned={earned} active={active}
                unlock={acc.unlock}
                onClick={() => earned && onSetWorn(key)} />
            );
          })}
        </div>
      </div>

      <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
        letterSpacing: '0.18em', color: MID, marginTop: 14, textAlign: 'center', lineHeight: 1.6 }}>
        moki lebt, solange du lebst.<br/>
        keine ängste. keine alarme.
      </p>
    </div>
  );
}

function WardrobeSlot({ label, earned = true, active, unlock, onClick }) {
  return (
    <button onClick={onClick}
      style={{
        padding: '8px 6px', borderRadius: 2,
        border: `${active ? 1.5 : 1}px ${active ? 'solid' : 'dashed'} ${active ? INK : (earned ? LIGHT : MID)}`,
        background: active ? INK : 'transparent',
        color: active ? PAPER : (earned ? INK : MID),
        opacity: earned ? 1 : 0.45, textAlign: 'center',
      }}>
      <div style={{ fontFamily: '"Fraunces", serif', fontSize: 12, fontStyle: 'italic' }}>
        {earned ? label : '?'}
      </div>
      <div style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 7,
        letterSpacing: '0.12em', marginTop: 2, textTransform: 'uppercase',
        color: active ? LIGHT : MID, lineHeight: 1.3 }}>
        {earned ? (active ? 'getragen' : 'tippen') : unlock}
      </div>
    </button>
  );
}

// ============================================================================
// COMPOSE SHEET — overlay for creating todos / habits / events
// Hardware: LVGL lv_keyboard with custom minimal layout.
// ============================================================================
function ComposeSheet({ compose, setCompose, onSave }) {
  if (!compose) return null;

  const titleText = { todo: 'neue aufgabe', habit: 'neue gewohnheit', event: 'neuer termin' }[compose.kind];
  const kind = compose.kind;

  // Which field is focused — drives what the keyboard types into
  const focus = compose.focus || 'title';
  const setField = (field, value) =>
    setCompose({ ...compose, [field]: value });

  const onKey = (key) => {
    const val = compose[focus] || '';
    if (key === 'BACK') setField(focus, val.slice(0, -1));
    else if (key === 'SPACE') setField(focus, val + ' ');
    else setField(focus, val + key);
  };

  const canSave = (compose.title || '').trim().length > 0;

  return (
    <div className="absolute inset-0 flex flex-col"
      style={{ background: PAPER, zIndex: 20 }}>
      {/* Header */}
      <div className="flex items-center justify-between px-5 py-3"
        style={{ borderBottom: `1px dashed ${LIGHT}` }}>
        <button onClick={() => setCompose(null)}
          style={{ color: MID, fontFamily: '"JetBrains Mono", monospace',
            fontSize: 10, letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          abbrechen
        </button>
        <span style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
          letterSpacing: '0.25em', color: DARK, textTransform: 'uppercase' }}>
          {titleText}
        </span>
        <button onClick={() => canSave && onSave(compose)} disabled={!canSave}
          style={{ color: canSave ? INK : MID, opacity: canSave ? 1 : 0.4,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.2em', textTransform: 'uppercase' }}>
          sichern
        </button>
      </div>

      {/* Form fields */}
      <div className="px-5 py-4 flex-1 flex flex-col gap-3 overflow-y-auto">
        <Field label="titel" value={compose.title || ''} focused={focus === 'title'}
          onFocus={() => setCompose({ ...compose, focus: 'title' })}
          placeholder="..." />

        {(kind === 'todo' || kind === 'event') && (
          <Field label="beschreibung" value={compose.desc || ''} focused={focus === 'desc'}
            onFocus={() => setCompose({ ...compose, focus: 'desc' })}
            placeholder="optional" />
        )}

        {kind === 'todo' && (
          <>
            {/* Category picker */}
            <div>
              <FieldLabel text="kategorie" />
              <div className="flex flex-wrap gap-1.5 mt-1.5">
                {Object.entries(CATEGORIES).map(([k, c]) => {
                  const active = compose.cat === k;
                  return (
                    <button key={k} onClick={() => setCompose({ ...compose, cat: k })}
                      style={{ padding: '5px 9px', borderRadius: 2,
                        border: `1px ${active ? 'solid' : 'dashed'} ${active ? INK : LIGHT}`,
                        background: active ? INK : 'transparent',
                        color: active ? PAPER : DARK,
                        fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                        letterSpacing: '0.15em', textTransform: 'uppercase' }}>
                      {c.mark} {c.label}
                    </button>
                  );
                })}
              </div>
            </div>
            {/* Deadline quick picks */}
            <div>
              <FieldLabel text="bis wann" />
              <div className="flex gap-1.5 mt-1.5">
                {['heute', 'morgen', 'diese woche', null].map((d, i) => {
                  const active = compose.deadline === d;
                  return (
                    <button key={i} onClick={() => setCompose({ ...compose, deadline: d })}
                      style={{ flex: 1, padding: '6px 4px', borderRadius: 2,
                        border: `1px ${active ? 'solid' : 'dashed'} ${active ? INK : LIGHT}`,
                        background: active ? INK : 'transparent',
                        color: active ? PAPER : DARK,
                        fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                        letterSpacing: '0.1em', textTransform: 'uppercase' }}>
                      {d || 'kein termin'}
                    </button>
                  );
                })}
              </div>
            </div>
            {/* Recurring toggle */}
            <div>
              <FieldLabel text="wiederkehrend" />
              <div className="flex gap-1.5 mt-1.5">
                {[
                  { v: null,      l: 'einmalig'    },
                  { v: 'daily',   l: 'täglich'     },
                  { v: 'weekly',  l: 'wöchentlich' },
                ].map(opt => {
                  const active = compose.recurring === opt.v;
                  return (
                    <button key={String(opt.v)} onClick={() => setCompose({ ...compose, recurring: opt.v })}
                      style={{ flex: 1, padding: '6px 4px', borderRadius: 2,
                        border: `1px ${active ? 'solid' : 'dashed'} ${active ? INK : LIGHT}`,
                        background: active ? INK : 'transparent',
                        color: active ? PAPER : DARK,
                        fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                        letterSpacing: '0.1em', textTransform: 'uppercase' }}>
                      {opt.l}
                    </button>
                  );
                })}
              </div>
            </div>
          </>
        )}

        {kind === 'event' && (
          <>
            <Field label="ort" value={compose.place || ''} focused={focus === 'place'}
              onFocus={() => setCompose({ ...compose, focus: 'place' })}
              placeholder="wo?" />
            <div>
              <FieldLabel text="sichtbarkeit" />
              <div className="flex gap-1.5 mt-1.5">
                {[
                  { v: 'private', l: '◯ privat'   },
                  { v: 'friends', l: '◐ freund_innen' },
                  { v: 'public',  l: '◉ öffentlich' },
                ].map(opt => {
                  const active = (compose.kind_vis || 'private') === opt.v;
                  return (
                    <button key={opt.v} onClick={() => setCompose({ ...compose, kind_vis: opt.v })}
                      style={{ flex: 1, padding: '6px 4px', borderRadius: 2,
                        border: `1px ${active ? 'solid' : 'dashed'} ${active ? INK : LIGHT}`,
                        background: active ? INK : 'transparent',
                        color: active ? PAPER : DARK,
                        fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
                        letterSpacing: '0.1em', textTransform: 'uppercase' }}>
                      {opt.l}
                    </button>
                  );
                })}
              </div>
            </div>
          </>
        )}
      </div>

      {/* Keyboard */}
      <Keyboard onKey={onKey} />
    </div>
  );
}

function FieldLabel({ text }) {
  return (
    <p style={{ fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
      letterSpacing: '0.25em', color: MID, textTransform: 'uppercase' }}>
      {text}
    </p>
  );
}

function Field({ label, value, focused, onFocus, placeholder }) {
  return (
    <div>
      <FieldLabel text={label} />
      <button onClick={onFocus} className="w-full text-left mt-1 py-2"
        style={{ borderBottom: `${focused ? 1.5 : 1}px solid ${focused ? INK : LIGHT}`,
          fontFamily: '"Fraunces", serif', fontSize: 16,
          color: value ? INK : MID, fontStyle: value ? 'normal' : 'italic',
          minHeight: 28 }}>
        {value || placeholder}
        {focused && <span style={{ animation: 'caret-blink 1s step-end infinite' }}>▎</span>}
      </button>
    </div>
  );
}

// Minimal stylized keyboard — not a full QWERTY, but enough for quick entries
function Keyboard({ onKey }) {
  const rows = [
    ['q','w','e','r','t','z','u','i','o','p'],
    ['a','s','d','f','g','h','j','k','l','ä'],
    ['y','x','c','v','b','n','m','ö','ü','ß'],
  ];
  return (
    <div style={{ background: '#d6cfba', borderTop: `1px solid ${MID}`,
      padding: '6px 4px 8px' }}>
      {rows.map((row, ri) => (
        <div key={ri} className="flex justify-center gap-1 mb-1">
          {row.map(k => (
            <button key={k} onClick={() => onKey(k)}
              style={{ flex: 1, padding: '7px 0', maxWidth: 32,
                background: PAPER, border: `1px solid ${MID}`,
                fontFamily: '"Fraunces", serif', fontSize: 14, color: INK,
                borderRadius: 2 }}>
              {k}
            </button>
          ))}
        </div>
      ))}
      <div className="flex justify-center gap-1">
        <button onClick={() => onKey('BACK')}
          style={{ flex: 1, maxWidth: 48, padding: '7px 0',
            background: LIGHT, border: `1px solid ${MID}`,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
            letterSpacing: '0.1em', color: DARK, borderRadius: 2 }}>
          ⌫
        </button>
        <button onClick={() => onKey('SPACE')}
          style={{ flex: 3, padding: '7px 0',
            background: PAPER, border: `1px solid ${MID}`,
            fontFamily: '"JetBrains Mono", monospace', fontSize: 9,
            letterSpacing: '0.2em', color: MID, borderRadius: 2,
            textTransform: 'uppercase' }}>
          leerzeichen
        </button>
      </div>
    </div>
  );
}

// ============================================================================
// MAIN
// ============================================================================
export default function MokiDevice() {
  const [state, setState] = useState(createInitialState);
  const [screen, setScreen] = useState(S.HOME);
  const [stack, setStack] = useState([]);
  const [flash, setFlash] = useState(false);
  const [syncing, setSyncing] = useState(false);
  const [activeHabit, setActiveHabit] = useState(null);
  const [activeChat, setActiveChat] = useState(null);
  const [activeFriend, setActiveFriend] = useState(null);
  const [doTab, setDoTab] = useState('habits');
  const [readTab, setReadTab] = useState('book');
  const [mapTab, setMapTab] = useState('map');
  const [activeNote, setActiveNote] = useState(null);
  const [notesFolder, setNotesFolder] = useState('alle');
  // Handoff: null or { kind, label, payload }
  const [handoff, setHandoff] = useState(null);
  const [toast, setToast] = useState(null);
  // Compose sheet: null | { kind: 'todo'|'habit'|'event', title, desc, ... }
  const [compose, setCompose] = useState(null);

  const go = (next, payload) => {
    if (next === screen && !payload) return;
    if (next === S.FRIEND    && payload) setActiveFriend(payload);
    if (next === S.CHAT      && payload) setActiveChat(payload);
    if (next === S.NOTE_EDIT && payload) setActiveNote(payload);
    setFlash(true);
    setStack(s => [...s, screen]);
    setTimeout(() => { setScreen(next); setFlash(false); }, 160);
  };
  const back = () => {
    setFlash(true);
    setTimeout(() => {
      setScreen(stack[stack.length - 1] || S.HOME);
      setStack(s => s.slice(0, -1));
      setFlash(false);
    }, 160);
  };

  const triggerSync = () => {
    if (syncing) return;
    setSyncing(true);
    setTimeout(() => {
      setState(s => ({ ...s, sync: { ...s.sync, lastAt: Date.now() } }));
      setSyncing(false);
      setToast('sync · 2 neue moods');
      setTimeout(() => setToast(null), 2200);
    }, 1800);
  };

  useEffect(() => {
    const t = setInterval(() => setState(s => ({ ...s })), 1000);
    return () => clearInterval(t);
  }, []);

  // Habit: increment today's count AND update last history entry
  const incrementHabit = (id) => {
    setState(s => ({
      ...s,
      habits: s.habits.map(h => {
        if (h.id !== id) return h;
        const newCount = h.todayCount + 1;
        const newHistory = [...h.history];
        newHistory[newHistory.length - 1] = newCount;
        const newStreak = h.todayCount === 0 ? h.streak + 1 : h.streak;
        return { ...h, todayCount: newCount, history: newHistory, streak: newStreak };
      }),
    }));
    setToast('+1');
    setTimeout(() => setToast(null), 900);
  };
  const openHabit = (id) => { setActiveHabit(id); go(S.HABIT_DETAIL); };

  const toggleTodo = (id) => setState(s => ({
    ...s, todos: s.todos.map(t => t.id === id ? { ...t, done: !t.done } : t),
  }));

  const setMood = (id) => {
    setState(s => ({ ...s, activeMood: { id, sentAt: Date.now(), expires: 'today' } }));
    setToast('wird beim nächsten sync geteilt');
    setTimeout(() => setToast(null), 2200);
  };
  const clearMood = () => setState(s => ({ ...s, activeMood: null }));

  const setWorn = (acc) => setState(s => ({ ...s, pet: { ...s.pet, worn: acc } }));

  // Compose: start a new entry (todo / habit / event)
  const openCompose = (kind) => {
    const base = { kind, title: '', focus: 'title' };
    if (kind === 'todo')  setCompose({ ...base, desc: '', cat: 'home', deadline: null, recurring: null });
    if (kind === 'habit') setCompose({ ...base });
    if (kind === 'event') setCompose({ ...base, desc: '', place: '', kind_vis: 'private' });
  };

  const saveCompose = (c) => {
    setState(s => {
      if (c.kind === 'todo') {
        const id = Math.max(0, ...s.todos.map(t => t.id)) + 1;
        const newTodo = {
          id, title: c.title.trim(), desc: (c.desc || '').trim(),
          cat: c.cat || 'home', deadline: c.deadline || null,
          recurring: c.recurring || null, done: false,
        };
        return { ...s, todos: [...s.todos, newTodo] };
      }
      if (c.kind === 'habit') {
        const id = Math.max(0, ...s.habits.map(h => h.id)) + 1;
        const newHabit = {
          id, name: c.title.trim(),
          todayCount: 0, streak: 0, history: Array(84).fill(0),
        };
        return { ...s, habits: [...s.habits, newHabit] };
      }
      if (c.kind === 'event') {
        const id = Math.max(0, ...s.calendar.map(e => e.id)) + 1;
        const newEvent = {
          id, day: 2, hour: '—',
          title: c.title.trim(), place: (c.place || '').trim() || '—',
          kind: c.kind_vis || 'private',
        };
        return { ...s, calendar: [...s.calendar, newEvent] };
      }
      return s;
    });
    setCompose(null);
    setToast('gespeichert');
    setTimeout(() => setToast(null), 1400);
  };

  const cycleShare = () => {
    const modes = ['off', 'hourly', 'live'];
    setState(s => {
      const next = modes[(modes.indexOf(s.map.share) + 1) % modes.length];
      return { ...s, map: { ...s.map, share: next } };
    });
    setToast('sichtbarkeit geändert');
    setTimeout(() => setToast(null), 1400);
  };

  const openChat = (id) => {
    setState(s => ({
      ...s,
      chats: s.chats.map(c => c.id === id ? { ...c, unread: 0 } : c),
    }));
    go(S.CHAT, id);
  };

  const simulateSend = (chatId) => {
    // In firmware: open keyboard, let user type, send via MeshCore on next sync.
    // Here we just demo the flow.
    setToast('wird beim nächsten sync gesendet');
    setTimeout(() => setToast(null), 1800);
  };

  const openDMWith = (friendId) => {
    // Find or create direct chat with this friend
    const existing = state.chats.find(c => c.kind === 'direct' && c.members.includes(friendId));
    if (existing) openChat(existing.id);
    else {
      const friend = state.nearby.find(n => n.id === friendId);
      const id = 'dm-' + friendId;
      setState(s => ({
        ...s,
        chats: [...s.chats, {
          id, kind: 'direct', name: friend.name, members: [friendId],
          last: '— neu —', ts: 'jetzt', unread: 0, reset: null, messages: [],
        }],
      }));
      go(S.CHAT, id);
    }
  };

  // ---- Notes ----
  const openNote = (id) => go(S.NOTE_EDIT, id);
  const newNote = () => go(S.NOTE_NEW);
  const createFromTemplate = (templateId) => {
    const tmpl = state.note_templates.find(t => t.id === templateId);
    if (!tmpl) return;
    const id = 'n' + Date.now();
    setState(s => ({
      ...s,
      notes: [{
        id, title: '', body: tmpl.body, template: tmpl.label,
        updated_at: 'gerade', visibility: 'private', pinned: false,
      }, ...s.notes],
    }));
    // Replace the template-picker screen with the editor instead of pushing
    setActiveNote(id);
    setScreen(S.NOTE_EDIT);
  };
  const updateNote = (id, patch) => {
    setState(s => ({
      ...s,
      notes: s.notes.map(n => n.id === id
        ? { ...n, ...patch, updated_at: 'gerade' } : n),
    }));
  };
  const toggleNotePin = (id) => {
    setState(s => ({
      ...s,
      notes: s.notes.map(n => n.id === id ? { ...n, pinned: !n.pinned } : n),
    }));
  };
  const cycleNoteVisibility = (id) => {
    const order = ['private', 'friends', 'public'];
    setState(s => ({
      ...s,
      notes: s.notes.map(n => n.id === id
        ? { ...n, visibility: order[(order.indexOf(n.visibility) + 1) % 3] } : n),
    }));
  };
  const deleteNote = (id) => {
    setState(s => ({ ...s, notes: s.notes.filter(n => n.id !== id) }));
    setToast('gelöscht');
    setTimeout(() => setToast(null), 1400);
  };

  // ---- Handoff (BLE handover) ----
  const startHandoff = (kind, label, payload) => {
    setHandoff({ kind, label, payload });
  };
  const onHandoffConfirmed = (device) => {
    // In firmware: GATT write-response received, transfer complete.
    setToast(`übergeben an ${device.name}`);
    setTimeout(() => setToast(null), 1800);
  };

  // ---- Inbox (received via BLE handoff from others) ----
  const acceptInbox = (id) => {
    const item = state.inbox.find(i => i.id === id);
    if (!item) return;
    // In real app: turn item into proper note / save to places / etc.
    // For simulator: just remove from inbox with toast.
    setState(s => ({ ...s, inbox: s.inbox.filter(i => i.id !== id) }));
    setToast(`„${item.preview}" gespeichert`);
    setTimeout(() => setToast(null), 1600);
  };
  const dismissInbox = (id) => {
    setState(s => ({ ...s, inbox: s.inbox.filter(i => i.id !== id) }));
  };

  const screens = {
    [S.HOME]:         <HomeScreen         state={state} go={go} />,
    [S.DO]:           <DoScreen           state={state}
                         onIncrementHabit={incrementHabit}
                         onOpenHabit={openHabit}
                         onToggleTodo={toggleTodo}
                         onAddTodo={() => openCompose('todo')}
                         onAddHabit={() => openCompose('habit')}
                         onAddEvent={() => openCompose('event')}
                         tab={doTab} setTab={setDoTab} />,
    [S.HABIT_DETAIL]: <HabitDetailScreen  state={state} habitId={activeHabit} onBack={back} />,
    [S.CAL]:          <CalendarScreen     state={state} onAddEvent={() => openCompose('event')} />,
    [S.READ]:         <ReadScreen         state={state} tab={readTab} setTab={setReadTab}
                         onOpenNote={openNote} onNewNote={newNote}
                         onToggleNotePin={toggleNotePin}
                         folderFilter={notesFolder} setFolderFilter={setNotesFolder}
                         onHandoffBook={(b) => startHandoff('book', `${b.book} · ${b.author}`, b)} />,
    [S.NOTE_NEW]:     <TemplatePickerScreen state={state}
                         onPick={createFromTemplate}
                         onPickNew={() => { /* stub: user's own template editor */ }}
                         onBack={back} />,
    [S.NOTE_EDIT]:    <NoteEditorScreen   state={state} noteId={activeNote}
                         onBack={back}
                         onUpdate={updateNote}
                         onTogglePin={toggleNotePin}
                         onCycleVisibility={cycleNoteVisibility}
                         onDelete={deleteNote}
                         onHandoff={(n) => startHandoff('note', n.title || 'ohne titel', n)} />,
    [S.INBOX]:        <InboxScreen        state={state} onBack={back}
                         onAccept={acceptInbox} onDismiss={dismissInbox} />,
    [S.READER]:       <ReaderScreen       state={state} />,
    [S.FEED]:         <FeedScreen         state={state} />,
    [S.MAP]:          <MapTabsScreen      state={state} tab={mapTab} setTab={setMapTab} go={go}
                         onCycleShare={cycleShare}
                         onHandoffHere={(p) => startHandoff('place', p.name, p)} />,
    [S.NEARBY]:       <NearbyScreen       state={state} go={go} />,
    [S.CHATS]:        <ChatsScreen        state={state} onOpenChat={openChat} />,
    [S.CHAT]:         <ChatScreen         state={state} chatId={activeChat} onBack={back} onSend={simulateSend} />,
    [S.FRIEND]:       <FriendProfileScreen state={state} friendId={activeFriend} onBack={back} onOpenDM={openDMWith} />,
    [S.MOOD]:         <MoodScreen         state={state} onSetMood={setMood} onClearMood={clearMood} onBack={back} />,
    [S.PROFILE]:      <ProfileScreen      state={state} onBack={back} />,
    [S.MOKI]:         <MokiScreen         state={state} onSetWorn={setWorn} onBack={back} />,
  };

  const dockVisible = ![S.HABIT_DETAIL, S.MOOD, S.PROFILE, S.MOKI,
                        S.CHAT, S.FRIEND, S.NOTE_NEW, S.NOTE_EDIT, S.INBOX].includes(screen);

  return (
    <div className="min-h-screen w-full flex flex-col items-center justify-center p-4 md:p-8"
      style={{
        background: 'radial-gradient(circle at 30% 20%, #2a2620 0%, #14110d 70%)',
        fontFamily: '"Fraunces", serif' }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=Fraunces:ital,opsz,wght@0,9..144,300..700;1,9..144,300..700&family=JetBrains+Mono:wght@400;500&display=swap');
        @keyframes moki-breathe { 0%,100% { transform: scale(1); } 50% { transform: scale(1.03); } }
        @keyframes eink-flash   { 0% { opacity: 0; } 40% { opacity: 1; } 100% { opacity: 0; } }
        @keyframes sync-pulse   { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }
        @keyframes toast-in     { 0% { opacity: 0; transform: translateY(8px); } 100% { opacity: 1; transform: none; } }
        @keyframes ping-ring    { 0% { opacity: 0.8; transform: scale(0.3); } 100% { opacity: 0; transform: scale(1.6); } }
        @keyframes caret-blink  { 0%, 50% { opacity: 1; } 51%, 100% { opacity: 0; } }
        @keyframes handoff-ring { 0% { transform: scale(0.6); opacity: 0.8; } 100% { transform: scale(1.3); opacity: 0; } }
        @keyframes spin         { to { transform: rotate(360deg); } }
      `}</style>

      <div className="relative" style={{
        padding: '28px 18px', borderRadius: 28,
        background: 'linear-gradient(145deg, #1f1c17 0%, #0e0c09 100%)',
        boxShadow: '0 30px 80px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.05)',
        width: 'min(380px, 100%)' }}>
        <div className="mx-auto mb-3" style={{ width: 48, height: 3, background: '#2a2620', borderRadius: 2 }} />

        <div className="relative overflow-hidden" style={{
          aspectRatio: '540 / 960', background: PAPER, borderRadius: 4,
          boxShadow: 'inset 0 0 0 1px rgba(0,0,0,0.25), inset 0 2px 8px rgba(0,0,0,0.15)' }}>
          <div className="absolute inset-0 pointer-events-none" style={{
            background: `repeating-linear-gradient(45deg, transparent 0 2px, rgba(0,0,0,0.015) 2px 3px)`,
            mixBlendMode: 'multiply' }} />

          <div className="relative h-full flex flex-col">
            <StatusBar state={state} onForceSync={triggerSync} syncing={syncing} />
            <div className="flex-1 flex flex-col overflow-hidden">
              {screens[screen]}
            </div>
            {dockVisible && <Dock current={screen} onSelect={go} />}
          </div>

          {toast && (
            <div className="absolute left-1/2 -translate-x-1/2"
              style={{ bottom: dockVisible ? 70 : 24, background: INK, color: PAPER,
                fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
                letterSpacing: '0.2em', textTransform: 'uppercase',
                padding: '8px 14px', borderRadius: 2,
                animation: 'toast-in 260ms ease-out' }}>
              {toast}
            </div>
          )}

          {flash && (
            <div className="absolute inset-0 pointer-events-none" style={{
              background: INK, animation: 'eink-flash 160ms ease-out' }} />
          )}

          {/* Compose sheet overlays entire screen */}
          <ComposeSheet compose={compose} setCompose={setCompose} onSave={saveCompose} />

          {/* Handoff sheet — BLE proximity ritual */}
          <HandoffSheet handoff={handoff} setHandoff={setHandoff} onConfirm={onHandoffConfirmed} />
        </div>

        <div className="absolute top-20 -right-1 w-1 h-10 rounded" style={{ background: '#2a2620' }} />
        <div className="absolute top-36 -right-1 w-1 h-14 rounded" style={{ background: '#2a2620' }} />
        <div className="absolute top-28 -left-1 w-1 h-6 rounded" style={{ background: '#2a2620' }} />
      </div>

      <p className="mt-6 text-center max-w-md px-4" style={{
        fontFamily: '"JetBrains Mono", monospace', fontSize: 10,
        letterSpacing: '0.18em', color: '#8a8373', textTransform: 'uppercase', lineHeight: 1.7 }}>
        moki · v0.11<br/>
        sammelbox auf heim · ort übergeben auf karte<br/>
        übergeben via bluetooth, ~20cm
      </p>
    </div>
  );
}
