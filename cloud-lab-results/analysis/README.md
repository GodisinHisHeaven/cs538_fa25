To use the script you need to have python installed.

Make sure you are in the analysis directory first before following the rest of the instructions below.

```
python3 -m .venv venv 
source .venv/bin/activate
pip install -r requirements.txt

python3 analyze_pcap.py ../iperf3_trace_logs_parallel
python3 analyze_pcap.py ../iperf3_trace_logs_parallel_baseline
```
