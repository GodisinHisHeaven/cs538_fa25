To use the script you need to have python installed.

Make sure you are in the analysis directory first before following the rest of the instructions below.

__The current rtt_analysis.json was obtained using TShark (Wireshark) 4.6.0 (Git commit 35a92c3b364a). The results may differ if an older version of TShark is used.__

```
python3 -m .venv venv 
source .venv/bin/activate
pip install -r requirements.txt

python3 analyze_pcap.py ../iperf3_trace_logs_parallel
python3 analyze_pcap.py ../iperf3_trace_logs_parallel_baseline
```
