awk -F, 'NR==2{t0=$1} NR>1{printf "%5.1f %-4s %4.0f%% ipc=%s llc=%s dtlb=%s\n",$1-t0,$3,$4*100,$5,$6,$7}' /tmp/c.csv > /tmp/c2.txt
echo "--- calm phase (t 2-52 s): state counts"
awk '$1>=4 && $1<52 {print $2}' /tmp/c2.txt | sort | uniq -c
echo "--- thrash phase (t 53-72 s): state counts"
awk '$1>=53 && $1<72 {print $2}' /tmp/c2.txt | sort | uniq -c
echo "--- samples"
awk 'NR%12==3' /tmp/c2.txt
