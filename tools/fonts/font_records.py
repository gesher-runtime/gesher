"""Bounded ASCII diagnostic records; full JSON payload is reconstructible from chunks."""
import hashlib,json

def emit(stage,value):
 payload=json.dumps(value,sort_keys=True,ensure_ascii=True,separators=(',',':'))
 digest=hashlib.sha256(payload.encode()).hexdigest();parts=[payload[i:i+320] for i in range(0,len(payload),320)] or ['']
 for index,part in enumerate(parts):
  print(stage+' '+json.dumps({'record_sha256':digest,'part':index+1,'parts':len(parts),'payload':part},sort_keys=True,ensure_ascii=True),flush=True)
