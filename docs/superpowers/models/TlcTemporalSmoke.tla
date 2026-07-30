-------------------- MODULE TlcTemporalSmoke --------------------

VARIABLE x

Init == x = 0
Next == x' = x
Spec == Init /\ [][Next]_x

EventuallyTrue == <> TRUE

=============================================================================
