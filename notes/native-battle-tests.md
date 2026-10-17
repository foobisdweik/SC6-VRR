# Broader tests for native battle logic

Requested on 2026-10-16.

The next milestone tests connected battle behavior across recorded sequences for both fighters.
More isolated calls or additional combinations of simple numbers do not satisfy this requirement.
Metal rendering, HDR, interpolation, and changes to game timing remain deferred.

## Required scenarios

| Scenario | Behavior to record and compare |
| --- | --- |
| Movement and spacing | Both fighters walk, retreat, and move sideways. Record positions and changes in facing. |
| Airborne movement | Record takeoff, movement while airborne, and landing. Require captured airborne states. |
| Attacks | Record attacks that miss, attacks that hit, and attacks that are blocked. |
| Hits and recovery | Record pushback, the period when a fighter cannot act after a hit, knockdown, and recovery. |
| Freeze and slow motion | Record entry, repeated updates, and exit while preserving the original timing behavior. |
| Resets | Record a Training reset or round transition and check how the recorded state changes. |

A scenario counts as covered only when the recording contains the required state transitions.
A request to perform an action is not evidence that the action occurred in the capture.
Report missing scenarios and unsupported branches explicitly.

## Connected comparison

Identify a section of the character update that includes several functions and can be reconstructed with understood dependencies.
Verify the actual call order from the executable before choosing that section.
Use the running game to record its inputs, outputs, and transitions across consecutive updates.

The comparison must preserve computed state across the recovered section and across the updates it can execute independently.
Do not replace computed outputs with the next expected state to hide accumulated differences.
When unrecovered code changes an input, record that change as an external dependency and identify its boundary.
Such a boundary limits what the sequence proves.

Compare the fields that the recovered section changes, including both fighters when their behavior is linked.
Check branch outcomes and event order where the recorder can establish them.
Retain exact floating-point comparisons where the original operation order can be preserved.
Investigate a difference before changing the comparison rule.

## Acceptance

1. Record consecutive updates and identify missing records or changes in the active fighter objects.
2. Exercise each applicable scenario through connected recovered functions.
3. Compare the reconstructed outputs with the outputs produced by the original game.
4. Report the first differing update, affected fields, and dependency boundary when a comparison fails.
5. Report coverage separately from numerical agreement and performance.
6. Keep recovered code and game data outside Git.

The previous 64-call movement capture remains a useful preliminary check.
It contains isolated calls and no airborne states, so it does not meet this milestone.

## Initial investigation

The existing documentation identifies `0x14034da70` as the main character update.
Its listed sequence includes hit-state handling followed by movement integration.
Inspect this caller and its dependencies before implementing the broader recorder.
No broader sequence has passed yet.
