#!/bin/bash
# Poolmap_sc_ae.sh — bundled H3/XConn switch pool-map for the SC26 AE.
# Cloned verbatim from the H3's /eMMCRS/Poolmap.sh (the paper's 4x128G,
# 512G-per-host, ports 5/21/4/20 -> EP 8/9/10/11 = g1/g2/g3/g4 mapping).
# cxl_remap.sh pushes THIS to /eMMCRS/Poolmap_sc_ae.sh and runs it, so the
# remap does not depend on whatever /eMMCRS/Poolmap.sh currently holds.
# Fetched 2026-07-07. Re-clone if the module population changes.


echo "Turn off cxl switch" 
bmc_fc3 apollo_power_control off
sleep 5
echo "Turn on cxl switch"
bmc_fc3 apollo_power_control on
sleep 5
echo "Call xconn-control probe"
xconn-control probe
 
# memory module population:
# 128G DDR5 Samsumg: 2 18 
# 128G DDR4 Micron : 4 20 5 21
# 256G DDR4 Micron : 

echo "Set up Downstream Port 18 to Endpoint port 8 (G1)"
xconn-control poolmap 5  128 0 8    0x4080000000 0 
xconn-control poolmap 21 128 0 8    0x6080000000 1
xconn-control poolmap 4  128 0 8    0x8080000000 2 
xconn-control poolmap 20 128 0 8    0xa080000000 3 
xconn-control sethdmsize 8 512

#xconn-control poolmap 2  128 0 8    0x4080000000 0 
#xconn-control poolmap 18 128 0 8    0x6080000000 1
#xconn-control sethdmsize 8 256


echo "Set up Downstream Port 18 to Endpoint port 9 (G2)"
xconn-control poolmap 5  128 0 9    0x4080000000 0 
xconn-control poolmap 21 128 0 9    0x6080000000 1
xconn-control poolmap 4  128 0 9    0x8080000000 2 
xconn-control poolmap 20 128 0 9    0xa080000000 3 
xconn-control sethdmsize 9 512

#xconn-control poolmap 2  128 0 9    0x4080000000 0 
#xconn-control poolmap 18 128 0 9    0x6080000000 1
#xconn-control sethdmsize 9 256


echo "Set up Downstream Port 2 to Endpoint port 10 (G3)"
xconn-control poolmap 5  128 0 10    0x4080000000 0 
xconn-control poolmap 21 128 0 10    0x6080000000 1
xconn-control poolmap 4  128 0 10    0x8080000000 2 
xconn-control poolmap 20 128 0 10    0xa080000000 3 
xconn-control sethdmsize 10 512

echo "Set up Downstream Port 2 to Endpoint port 11 (G4)"
xconn-control poolmap 5  128 0 11    0x4080000000 0 
xconn-control poolmap 21 128 0 11    0x6080000000 1
xconn-control poolmap 4  128 0 11    0x8080000000 2 
xconn-control poolmap 20 128 0 11    0xa080000000 3 
xconn-control sethdmsize 11 512

echo "Done"