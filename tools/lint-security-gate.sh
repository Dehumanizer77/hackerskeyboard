#!/bin/bash
# Fails when Android lint reports a Security-category issue.
#
# The lint run itself cannot be the gate: upstream carries several hundred
# pre-existing ExtraTranslation, NamespaceTypo and ResAuto findings, some of
# them fatal-severity, so app/build.gradle keeps abortOnError off - otherwise
# nobody would run lint at all. What must never regress is the security set, so
# that is checked here, against lint's own category attribute rather than a
# hand-maintained list of issue ids.
#
#   tools/lint-security-gate.sh [lint-results.xml]
#
# With no argument it runs :app:lintRelease first and checks its report.
set -uo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPORT="${1:-}"

if [ -z "$REPORT" ]; then
    REPORT="$HERE/app/build/reports/lint-results-release.xml"
    (cd "$HERE" && ./gradlew --console=plain :app:lintRelease) || {
        echo "lint failed to run"; exit 2; }
fi

if [ ! -f "$REPORT" ]; then
    echo "no lint report at $REPORT"
    exit 2
fi

python3 - "$REPORT" <<'PY'
import sys
import xml.etree.ElementTree as ET

report = sys.argv[1]
root = ET.parse(report).getroot()

blocking = []
for issue in root.iter('issue'):
    if issue.get('category') != 'Security':
        continue
    if issue.get('severity') not in ('Error', 'Fatal'):
        continue
    loc = issue.find('location')
    where = ''
    if loc is not None:
        where = '%s:%s' % (loc.get('file', '?'), loc.get('line', '?'))
    blocking.append((issue.get('id'), issue.get('message', ''), where))

if not blocking:
    print('lint security gate: clean (%s)' % report)
    sys.exit(0)

print('lint security gate: %d blocking issue(s)' % len(blocking))
for issue_id, message, where in blocking:
    print('  [%s] %s' % (issue_id, message))
    if where:
        print('      %s' % where)
sys.exit(1)
PY
