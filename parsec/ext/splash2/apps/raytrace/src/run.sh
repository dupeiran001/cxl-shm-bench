#!/bin/sh

# Description:
#
# Runs the benchmark with a minimal problem size in a multiprocessor
# simulator.
#
# Usage:
#
# ./run.sh [ARCH [PARTYPE [NUMPROCS]]]]
#

#Some default values
TARGET=raytrace

#Arguments
if [ -n "$1" ]
then
  NUMPROCS="$1"
fi

if [ -n "$2" ]
then
  INPUTSIZE="$2"
fi


if [ -n "$3" ]
then
  echo "Error: Too many arguments!"
  echo 
  head -n11 $0 | tail -n9 | sed 's/#//'
  exit 1
fi


#Determine program name, file names & arguments
case "${INPUTSIZE}" in 
"test"	) 
	PROGARGS="-s -p$NUMPROCS teapot.env";;
"simdev"	) 
	PROGARGS="-s -p$NUMPROCS teapot.env";;
"simsmall"	) 
	PROGARGS="-s -p$NUMPROCS teapot.env";;
"simmedium"	) 
	PROGARGS="-s -p$NUMPROCS balls4.env";;
"simlarge"	) 
	PROGARGS="-s -p$NUMPROCS balls4.env";;
"native"	) 
	PROGARGS="-s -p$NUMPROCS car.env";;
*)  
	echo "Input size error"
	exit 1;;
esac
 

PROG="${PARSECDIR}/ext/splash2/apps/${TARGET}/inst/${PARSECPLAT}/bin/${TARGET}"


#Some tests
if [ ! -x "$PROG" ]
then
  echo "Error: Binary ${PROG} does not exist!"
  exit 1
fi


#Execution
SRC_DIR="${PARSECDIR}/ext/splash2/apps/${TARGET}/src"
for f in "${SRC_DIR}"/*.env "${SRC_DIR}"/*.geo
do
  if [ -f "$f" ]
  then
    base=$(basename "$f")
    if [ ! -f "$base" ]
    then
      cp "$f" "$base"
    fi
  fi
done

echo Preparing input files...

RUN="$PROG $PROGARGS"

echo "Running $RUN:"
eval $RUN

