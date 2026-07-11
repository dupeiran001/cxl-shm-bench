#!/bin/sh

# Description:
#
# Runs the benchmark with a minimal problem size in a multiprocessor
# simulator.
#
# Usage:
#
# ./run.sh [NUMPROCS]
#

#Some default values
TARGET=water_nsquared

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
INPUTFILE="input_${NUMPROCS}"
INPUTTEMPLATE="input.template"
PROGARGS="${NUMPROCS} < ${INPUTFILE}"
PROG="${PARSECDIR}/ext/splash2/apps/${TARGET}/inst/${PARSECPLAT}/bin/${TARGET}"


#Some tests
if [ ! -x "$PROG" ]
then
  echo "Error: Binary ${PROG} does not exist!"
  exit 1
fi


#Execution
if [ -f "${INPUTTEMPLATE}" ]
then
  echo Generating input file ${INPUTFILE}...
  cat ${INPUTTEMPLATE} | sed s/NUMPROCS/$NUMPROCS/ > ${INPUTFILE}
elif [ -f "${INPUTFILE}" ]
then
  echo Using existing input file ${INPUTFILE}...
else
  echo Generating default input file ${INPUTFILE}...
  cat > "${INPUTFILE}" <<EOF
1e-15
512
1
6
0
0
1
0
${NUMPROCS}
6.2
EOF
fi

SRC_RANDOM="${PARSECDIR}/ext/splash2/apps/${TARGET}/src/random.in"
if [ -f "${SRC_RANDOM}" ] && [ ! -f "random.in" ]
then
  cp "${SRC_RANDOM}" "random.in"
fi

RUN="$PROG $PROGARGS"

echo "Running $RUN:"
eval $RUN
