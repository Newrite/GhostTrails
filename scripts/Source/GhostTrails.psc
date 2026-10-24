Scriptname GhostTrails Hidden

; Positive ID on admission; negative Result code on rejection.
; RGB=-1,-1,-1 selects the complete default appearance (including alpha/intensity/rim).
Int Function Spawn(Actor target, Int count = 1, Float interval = 0.05, Float lifetime = 0.5, Float red = -1.0, Float green = -1.0, Float blue = -1.0, Float alpha = 0.5, Float intensity = 2.0, Float rimPower = 2.0, Float retryWindow = 1.0, Float spawnWindow = 0.0) Global Native
Int Function Prepare(Actor target, Int slots = 4) Global Native

; 0 Queued, 1 Running, 2 Completed, 3 Partial, 4 Failed, 5 Canceled.
; Negative Result code if the ID is unknown.
Int Function GetRequestState(Int requestID) Global Native
; [state, lastFailure, spawned, missed, attempts, requested], or [-Result].
Int[] Function GetStatus(Int requestID) Global Native

; Returns Result: 0 Accepted, 1 InvalidArgument, 2 NotReady, 3 Capacity,
; 4 NotFound, 5 Finished. These changes apply to future emissions.
Int Function SetAppearance(Int requestID, Float red, Float green, Float blue, Float alpha = 0.5, Float intensity = 2.0, Float rimPower = 2.0) Global Native
Int Function SetDefaultAppearance(Float red, Float green, Float blue, Float alpha = 0.5, Float intensity = 2.0, Float rimPower = 2.0) Global Native
Int Function Cancel(Int requestID) Global Native
Int Function Release(Int requestID) Global Native
