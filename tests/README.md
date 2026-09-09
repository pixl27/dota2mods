# GC receiver regression checks

Run `tests\run_tests.bat` from Windows with Visual Studio C++ Build Tools installed.
The script finds the x64 compiler and builds four standalone test executables.
It also uses Python for the cache-repair tests.

`gc_protocol_test.cpp` checks hand-written Dota wire fixtures, nested welcome
caches, account isolation, preservation of existing records and routing headers,
invalid input, buffer size negotiation, bounded retries, and a 15,000-item catalog.
It also validates 2,000 deterministic malformed-input samples.

`gc_loadout_test.cpp` checks native single/batched equip requests against literal
wire fixtures, slot zero, replacement, unequip, styles, routing the job response,
cache versions/services, refresh persistence, batch atomicity, owned-item
passthrough, and reconciling subsequent real item updates with local previews.

`gc_receiver_test.cpp` exercises the actual production receiver against a fake GC
and mocked hook operations. It checks immediate refresh after attachment, timeout
and retry, message ordering when a destination is too small, pausing with a pending
message, API failures, hook failure rollback, and diagnostic state transitions.
It also verifies that equip handling remains active after refresh, local item
IDs never reach the fake Steam send function, updates precede acknowledgements,
local replies honor buffer sizing, and pause drains replies and outstanding
notifications before disabling all five hooks.

The simulated callback pump reads one packet per message-available event and
never polls the GC on its own. It verifies a single equip and ten successive
equips with no network activity, update/ack delivery in the same pump, yielding
during a 40-equip burst (at most 32 local notifications per pump), priority for
real GC connection messages, correct callback payload/free pairing, isolation
of other Steam pipes, nested-pump handling, bounded retries when notifications
are ignored, pending-age diagnostics, and safe pause with an outstanding event.
It writes only `build/gc_receiver_test.log`; it does not load Dota, use a Steam
session, install real detours, or send network requests.

Passing these checks does not prove that a live Dota build accepts the modified
cache or that items can be equipped. Runtime verification still requires the
in-game receiver status and inspection of the local inventory.

Receiver v5 additionally tests service-0/service-1 isolation (including an empty
econ cache), saved-preview detection, account isolation, retry/resume without
another full catalog reload, and duplicate-instance rejection. The Python
repair tests use temporary fixtures under `build`, verify backup integrity and
preservation of clean caches, and reject repair while Dota is running.
