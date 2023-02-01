    .ORIG x0100
    TRAP xFF
    INF_LOOP:
        BR INF_LOOP
    .END