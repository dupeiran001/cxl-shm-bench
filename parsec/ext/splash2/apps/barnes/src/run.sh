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
TARGET=barnes

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

INPUTTEMPLATE="input.template"
INPUTFILE="input_${NUMPROCS}"
RUNINPUT="${PARSECDIR}/ext/splash2/apps/${TARGET}/run/${INPUTFILE}"
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
elif [ -f "${RUNINPUT}" ]
then
  echo Copying ${RUNINPUT} to ${INPUTFILE}...
  cp "${RUNINPUT}" "${INPUTFILE}"
else
  echo Generating default input file ${INPUTFILE}...
  cat > "${INPUTFILE}" <<EOF

16384
123

0.025
0.05
1.0
2.0
4.0
0.075
0.25
${NUMPROCS}
EOF
fi

RUN="$PROG $PROGARGS"

echo "Running $RUN:"
eval $RUN
