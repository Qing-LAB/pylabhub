# Inbox sender identity comes from the proven key

Execution notes for task #83 slice 4 — the inbox plane.  Design
authority is HEP-CORE-0027 §3.5/§3.7 and HEP-CORE-0035 §4.9.5/§4.9.6.
**Nothing here is design.**  Everything below is already specified; this
file records only what to change, what was checked, and what was
considered and rejected.

## 1. The gap, in one paragraph

The inbox takes the sender's identity from the ZMQ routing frame, which
the sender writes itself (`hub_inbox_queue.cpp:567`).  That string
becomes the replay key (`:599`), the per-sender sequence key
(`:643-659`) and the name the receiving script sees (`:695` →
`role_api_base.cpp:4763` → `msg.sender_uid`).  Meanwhile the CURVE
handshake proves a key, ZAP replies with it as `User-Id`
(`zap_router.cpp:592`), and `InboxQueue::is_peer_allowed`
(`:806-820`) checks it and **returns `bool`** — the proven key is
established and thrown away on every connection.

HEP-CORE-0027 §3.7 already forbids this in a blockquote, using MUST, and
§3.5 names the function to use.  This is not a design question.

## 2. What the HEP already decided

- The routing id is an **ACK return address** and carries no authority
  (§3.5, §3.7).
- The sender's identity is the principal resolved from the proven key,
  and it MUST be used for all three of: the application-visible sender,
  the replay key, the per-sender sequence state (§3.7).
- Where a role has more than one side, admission takes **any** side
  (I-ROSTER-COMBINE-ADMIT) and naming takes the **first side in a fixed
  order, input before output**, logging a disagreement rather than
  refusing (I-ROSTER-COMBINE-NAME).
- Both questions resolve against the held snapshots — no consumer gets
  its own copy (I-ROSTER-ASK-DONT-COPY).

## 3. The changes

**(a) Role side — the naming combine.**  `RoleAPIBase::Impl` gains the
sibling of `roster_admits` (`role_api_base.cpp:370`), implementing the
§4.9.6 `name_of` pseudocode: sides in fixed order input-first, first
recogniser wins, and before returning, check whether another side
recognises the same key under a different uid and log if so.  It cannot
short-circuit: detecting the disagreement is the reason both sides are
consulted.

**(b) One binding, not two.**  `set_admission_authority` takes both
questions together so they cannot be bound from different snapshots —
or half-bound.  Five call sites exist and all must move:
`role_api_base.cpp:3432` (production) and
`datahub_broker_workers.cpp:1879`,
`hub_inbox_queue_workers.cpp:149`, `:1180`, `:1307` (tests).
`hub_inbox_queue_workers.cpp:1248` deliberately binds nothing and pins
the deny-all default — it stays unbound, and defines the default for
BOTH answers.

**(c) `recv_one` derives instead of reads.**
`AttestedKey::from_message(socket, parts[0])` then attribute.  No branch
on `nullopt`: `attribute_sender` takes `std::optional<AttestedKey>` and
answers `no_attestation` itself (`pubkey_origin.cpp:135`).  Anything but
`accepted` drops the frame.

**(d) Split the variable that does two jobs.**  `current_sender_id_`
(`:72`) feeds ACK routing (`:708`) AND the three identity uses.  The ACK
keeps the routing id; the other three take the attributed uid.  They are
the same string today and diverge the moment a client picks a routing id
that is not its uid, which is the whole point.

**(e) One comment is currently false.**  `hub_inbox_queue.hpp:82`
documents `sender_id` as "from ZMQ identity frame" — the defect written
down as though it were the design.

**(f) Test.**  A raw DEALER that proves its own key and presents another
role's uid as its routing id.  Today it is attributed as the label it
chose; after the change, as the key it proved.  One hub is enough — this
does not wait on the two-hub work in the tests TODO.
The production `InboxClient` cannot express this: it sets `ZMQ_IDENTITY`
to `sender_uid` (`hub_inbox_queue.hpp:330`).  Which is also the honest
scope statement — **stock clients always agree today, so nothing in a
running deployment is being mis-attributed.**  The exposure is to a
hand-rolled client, which is what an attacker writes.

## 4. Checked, and what it settled

- `admits()` and `resolve_()` hit the same table (`pubkey_origin.cpp:89`,
  `:96`), so a peer cannot be admitted under one answer and named under
  another.  No new coherence mechanism is needed.
- `attribute_sender` returns four verdicts, not two: `no_attestation`,
  `unknown_key`, **`kind_not_permitted`**, `accepted`.  The third
  refuses a federation peer whose key IS in the table (`:145`, "kind
  before uid, deliberately") — so a peer hub dialling a role inbox is
  refused a name, and that case is already handled rather than needing
  new code.
- Frame 0 is the right frame to read: libzmq stamps ZAP metadata on
  every part, and the control plane reads frame 0 for exactly this
  (`wire_envelope.cpp:302-306`).
- Nothing role-side calls `attribute_sender` today — verified by grep,
  so this is the first role-side use, not a second one.

**Not yet verified:** whether anything beyond `role_api_base.cpp:4763`
and `send_ack` reads `InboxItem::sender_id`.  Check before changing it.

## 5. Considered and rejected

**One function instead of two.**  Tempting: ZAP could ask "who is this
key?" and admit iff the answer is a name, giving literally one call.
Rejected because the two callers hold different things.  At ZAP time
there is no `AttestedKey` and cannot be — deciding whether to attest IS
the question being asked — so the ZAP entry point must take a key
string.  Collapsing them would mean passing bare strings at the message
path too, which discards the one type whose whole purpose is to prove
where a key came from (`attested_key.hpp`: minting outside
`from_message` "would produce an `AttestedKey` whose name is a lie").
Two callables bound in one call keeps both properties.

**A counter per verdict.**  Rejected as overdesign.  The queue already
carries five recv-side counters; attribution failures get **one**, with
the verdict named in the log line.  Four counters would be four things
to maintain for a distinction only a log reader needs.

**Refusing on a cross-hub naming disagreement.**  Rejected — and it is
worth recording that the obvious "safe" choice is the wrong one.
Fail-closed would let a broken or hostile hub silence a healthy one by
claiming its keys under wrong names.  The fixed input-before-output
order removes that lever, which is why §4.9.5 chose it.
