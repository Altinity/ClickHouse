# CA Merkle-store spec — distributed-systems / S3 correctness review (2026-06-07)

Adversarial review of `docs/superpowers/specs/2026-06-07-ca-merkle-store-design.md` using Lamport-style
happens-before analysis, deep state analysis, and failure injection. Focused on the two newest mechanisms (the
Keeper `epoch` cache and the then-current D6 write-ahead-intent / condemn-the-orphan path), since the hardened
EBR core was already validated by reviews R1–R3 and the TLC model.

## Verdict {#verdict}
**SOUND WITH MUST-FIX.** The four §6 hinges (session-alive self-fence, flush-`+`-then-advance + closed-epoch
fold barrier, fenced single deleter, decoupled generations) survive scrutiny. The breaks were all in the two new
mechanisms.

## Findings {#findings}

1. **[MUST-FIX] D6 "in-degree==0 guard *alone*" was over-stated.** In the decide→`+`-not-yet-durable window the
   fold legitimately reads 0; what actually protects a concurrent dedup-reuser is `safe_epoch > e_a` (its live
   lease) + its post-`+` resurrect-on-condemned step — the same machinery as every other node, not the in-degree
   guard. Risk: an implementer reading "no rescue, in-degree alone" might reclaim the orphan directly in R2,
   skipping the `safe_epoch` gate → loss. **Fix:** orphan reclaim must go through the full R4 gate with
   `e_a` = current epoch; drop "alone."
2. **[MUST-FIX] Intent key `leases/<epoch>/<key>` collides across writers** building the same content — owner
   attribution breaks, delete-on-commit is ambiguous, a concurrent committer can delete the orphan-uploader's
   intent. **Fix:** owner-disambiguate the path (`leases/<session_id>/<epoch>/<key>`).
3. **[SHOULD-FIX] Epoch cache late-`+`.** "Keeper never ahead of S3" was *verified true* — but only if nothing
   else ever writes the `epoch` znode (state it). The lagging-cache late-`+` case reduces to the proven CE-1
   *provided* the reappend rule is bound to the cache (cache is hint-only for `O_W`, never the authority for "is
   my `+`'s epoch still foldable-open").
4. **[SHOULD-FIX] Keeper-wipe sweep** needs the R3 K7 rule restated: age-test on the S3 object's
   last-modified / multipart-completion time + `Retention` > max op duration, else a pre-wipe in-flight upload
   completing post-wipe can be swept.
5. **[SHOULD-FIX] `e+2` is sufficient and `2` would be too few** (verified). Sufficiency is independent of cache
   lag, because `safe_epoch = min(O_W)` reads the writer's *published* (lagged) `O_W` and self-adjusts; state
   this so an implementer does not add a wrong "cache within 1 epoch" assumption.
6. **[NIT] Reader** needs an explicit restart-from-ref when `GET 404` + empty `LIST` (node reclaimed under a
   dropped ref).

Over-stated claims flagged: "in-degree alone" (Finding 1); "never ahead of S3" (conditional on sole writer of
the epoch znode, Finding 3); INV-S3-COMPLETE "loses nothing" (conditional on the sweep's age-basis + grace,
Finding 4); "lock-free" rests on the session-timeout assumption (already honest in §6.1).

## TLA+ properties suggested {#tla}
Add `leaseIntents` and the D6 actions; the Keeper-cache split (`s3Epoch` vs `keeperEpoch ≤ s3Epoch`); the
late-`+`-into-closed-epoch reappend under a *lagging* epoch source; the post-wipe sweep grace; multi-child commit
atomicity with the orphan path active.

## Disposition (what the spec did with this) {#disposition}
- The user **cut D6 entirely** (crash orphans → periodic Retention-guarded full-`LIST` sweep only). This dissolves
  Findings 1 and 2 (no intents, no rescue, no key). The sweep's age-basis + grace (Finding 4) was folded into §4.4
  / §7 / §9.
- The Keeper `epoch` cache was **kept**, with the **sole-writer rule** made normative (§3.2, §6.2, §9 D1) —
  Finding 3's condition. The reappend-binding (cache is hint-only) and the `e+2` lag-independence note (Finding 5)
  were added to §6.2.
- The reader restart-from-ref (Finding 6) was added to §4.3.
