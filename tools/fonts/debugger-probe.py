"""CI-only exact fresh harmless debugger child; no font/runtime qualification."""
import argparse,hashlib,importlib.util,json,os,posixpath,re,subprocess
from pathlib import Path
from font_records import emit
ROLE='CURRENT_SOURCE_NATIVE_DEBUGGER_PREFLIGHT_PROBE'
COMPILER_SHA='c9b86311ade81d53235c93fafabdf98328d094de344ea9a9038e0dab6695ee9f'
ARCHIVE_SHA='5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
PTHREAD_SHA='dbbbb3a59f5a1077346b69d4dd8ea85eeca9574bfbd2d170eaf91b5fe5b2f88e'
SOURCE='tests/fonts-debugger-preflight/probe.cc'
INPUTS=(SOURCE,'.github/workflows/fonts-debugger-preflight-component.yml',
 'tools/fonts/debugger-probe.py','tools/fonts/test-debugger-probe.py','tools/fonts/debugger-preflight.ps1','tools/fonts/test-debugger-preflight.ps1',
 'tools/fonts/font_records.py','tools/fonts/cli-link-evidence.py','tools/fonts/verify-standalone-link.py')
FILES=('probe.exe','probe.obj','probe.map','probe.imports.txt','probe-build.log')
FLAGS={'font_owner_qualified':False,'host_WCT_Windows2025_qualified':False,'private_CRT_owner_symbols_qualified':False,'full_qualification':False}

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def context():
 source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 assert source==os.environ['GITHUB_SHA'] and os.environ['GITHUB_REPOSITORY']=='gesher-runtime/gesher' and os.environ['GITHUB_REPOSITORY_ID']=='1426153186'
 assert re.fullmatch('[0-9]+',os.environ['GITHUB_RUN_ID']) and re.fullmatch('[1-9][0-9]*',os.environ['GITHUB_RUN_ATTEMPT'])
 return {'source':source,'run':os.environ['GITHUB_RUN_ID'],'attempt':os.environ['GITHUB_RUN_ATTEMPT'],'repository':os.environ['GITHUB_REPOSITORY'],'repository_id':os.environ['GITHUB_REPOSITORY_ID']}
def inputs():
 result={p:subprocess.check_output(['git','hash-object',p],text=True).strip() for p in INPUTS}
 assert all(subprocess.check_output(['git','rev-parse','HEAD:'+p],text=True).strip()==blob for p,blob in result.items())
 return result

def commands(compiler,source,obj,exe,mapping):
 return [[compiler,'-std=c++17','-O2','-g','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Werror','-c',source,'-o',obj],
         [compiler,obj,'-static','-s','-Wl,-Map,'+mapping,'-o',exe]]

def main_symbol(raw):
 # Standard AMD64 COFF (not BigObj), including auxiliary record skipping.
 assert len(raw)>=20 and int.from_bytes(raw[:2],'little')==0x8664
 sections=int.from_bytes(raw[2:4],'little');optional=int.from_bytes(raw[16:18],'little')
 pointer=int.from_bytes(raw[8:12],'little');count=int.from_bytes(raw[12:16],'little');strings=pointer+18*count
 assert sections>0 and optional==0 and pointer>=20+40*sections and strings+4<=len(raw)
 length=int.from_bytes(raw[strings:strings+4],'little');assert length>=4 and strings+length<=len(raw)
 found=[];i=0
 while i<count:
  item=raw[pointer+18*i:pointer+18*(i+1)];assert len(item)==18
  if item[:4]==bytes(4):
   offset=int.from_bytes(item[4:8],'little');assert 4<=offset<length
   end=raw.find(bytes(1),strings+offset,strings+length);assert end>=0;name=raw[strings+offset:end]
  else:name=item[:8].split(bytes(1),1)[0]
  if name==b'main':found.append((int.from_bytes(item[12:14],'little',signed=True),item[16],int.from_bytes(item[14:16],'little')))
  i+=1+item[17];assert i<=count
 assert len(found)==1 and 0<found[0][0]<=sections and found[0][1]==2 and found[0][2]&0x20,'Actual object lacks unique external function main'

def image(root):
 raw=(root/'probe.exe').read_bytes();assert raw[:2]==b'MZ' and len(raw)>0x40
 offset=int.from_bytes(raw[0x3c:0x40],'little');assert raw[offset:offset+4]==b'PE\0\0' and int.from_bytes(raw[offset+4:offset+6],'little')==0x8664
 main_symbol((root/'probe.obj').read_bytes())
 imports=(root/'probe.imports.txt').read_text(encoding='utf-8');assert all(re.search(r'\bSymbol: '+name+r'\b',imports) for name in ('getenv','getchar','puts','fflush'))
 assert not re.search(r'(?i)(jvm|dalvik|libart|JNI_OnLoad|FontEnvironment|libwinpthread.*\.dll)',imports)
 return imports

def parser():
 spec=importlib.util.spec_from_file_location('debugger_preflight_link_evidence',Path(__file__).with_name('cli-link-evidence.py'))
 module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def build(root,toolchain,archive):
 context();inputs()
 assert not root.exists() and sha(archive)==ARCHIVE_SHA
 root.mkdir(parents=True);root=root.resolve();toolchain=toolchain.resolve()
 assert toolchain.name=='llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64'
 compiler=toolchain/'bin/x86_64-w64-mingw32-clang++';assert sha(compiler)==COMPILER_SHA
 # Bind the driver's actual canonical archive paths, not a guessed layout.
 canonical={}
 for name in ('libpthread.a','libwinpthread.a'):
  actual=Path(subprocess.check_output([str(compiler),'-print-file-name='+name],text=True).strip()).resolve()
  assert actual.is_relative_to(toolchain) and sha(actual)==PTHREAD_SHA
  canonical[name]={'path':str(actual),'sha256':sha(actual)}
 obj=root/'probe.obj';exe=root/'probe.exe';mapping=root/'probe.map'
 build_commands=commands(str(compiler),str(Path(SOURCE).resolve()),str(obj),str(exe),str(mapping))
 chunks=[]
 for command in build_commands:
  emit('FONT_DEBUGGER_PROBE_COMMAND',command)
  result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  chunks.append(json.dumps(command)+'\n'+result.stdout)
  if result.stdout:print(result.stdout,flush=True)
  assert result.returncode==0,('Current harmless child compile/link failed',result.returncode)
 (root/'probe-build.log').write_text('\n'.join(chunks),encoding='utf-8')
 imports=subprocess.check_output([str(toolchain/'bin/llvm-readobj'),'--coff-imports',str(exe)],text=True)
 emit('FONT_DEBUGGER_PROBE_IMPORTS_RAW',imports);(root/'probe.imports.txt').write_text(imports,encoding='utf-8')
 image(root)
 symbols=subprocess.check_output([str(toolchain/'bin/llvm-nm'),'-j','--defined-only',str(obj)],text=True).splitlines();assert symbols.count('main')==1
 evidence=parser();rows=evidence.map_records(mapping.read_text(encoding='utf-8'));entry=evidence.entry(rows,'main',obj,root)
 record={'role':ROLE,'context':context(),'inputs':inputs(),'outputs':{p:sha(root/p) for p in FILES},'compiler_sha256':sha(compiler),'archive_sha256':sha(archive),'compiler_version':subprocess.check_output([str(compiler),'--version'],text=True),'canonical_pthread':canonical,'commands':build_commands,'entry':entry,**FLAGS}
 (root/'debugger-probe-provenance.json').write_text(json.dumps(record,sort_keys=True)+'\n',encoding='utf-8');verify(root)

def verify(root):
 assert root.is_dir() and all(p.is_file() and not p.is_symlink() for p in root.iterdir())
 assert {p.name for p in root.iterdir()}==set(FILES)|{'debugger-probe-provenance.json'}
 record=json.loads((root/'debugger-probe-provenance.json').read_text(encoding='utf-8'))
 assert set(record)=={'role','context','inputs','outputs','compiler_sha256','archive_sha256','compiler_version','canonical_pthread','commands','entry'}|set(FLAGS)
 assert record['role']==ROLE and record['context']==context() and record['inputs']==inputs() and record['outputs']=={p:sha(root/p) for p in FILES}
 assert record['compiler_sha256']==COMPILER_SHA and record['archive_sha256']==ARCHIVE_SHA
 assert all(record[k] is False for k in FLAGS) and set(record['canonical_pthread'])=={'libpthread.a','libwinpthread.a'} and all(v['sha256']==PTHREAD_SHA for v in record['canonical_pthread'].values())
 assert type(record['commands'])is list and len(record['commands'])==2
 first=record['commands'][0];assert type(first)is list and len(first)==13
 compiler=first[0];source=first[10];obj=first[12]
 assert all(type(value)is str and value.startswith('/') and '\\'not in value and posixpath.normpath(value)==value and '\x00'not in value for value in (compiler,source,obj))
 assert source.endswith('/'+SOURCE) and compiler.endswith('/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin/x86_64-w64-mingw32-clang++') and posixpath.basename(obj)=='probe.obj'
 producer_root=posixpath.dirname(obj)
 assert record['commands']==commands(compiler,source,obj,producer_root+'/probe.exe',producer_root+'/probe.map')
 rows=parser().map_records((root/'probe.map').read_bytes().decode('utf-8'))
 selected=sorted(set((rva,name,owner)for rva,name,owner in rows if name=='main'))
 assert len(selected)==1 and selected[0][0]>0 and posixpath.normpath(posixpath.join(producer_root,selected[0][2]))==obj
 assert record['entry']=={'symbol':'main','rva':selected[0][0],'object':obj,'identical_rows_coalesced':sum(name=='main'for _,name,_ in rows)-1}
 image(root)
 for key,value in record.items():emit('FONT_DEBUGGER_PROBE_'+key.upper(),value)
 emit('FONT_DEBUGGER_PROBE_MANIFEST',{'context':record['context'],'role':ROLE,'sha256':sha(root/'debugger-probe-provenance.json')})
 return record

def main():
 p=argparse.ArgumentParser();p.add_argument('mode',choices=('build','verify'));p.add_argument('--root',type=Path,required=True);p.add_argument('--toolchain',type=Path);p.add_argument('--archive',type=Path);a=p.parse_args()
 assert os.environ.get('GITHUB_ACTIONS')=='true','Native compilation/execution/artifact handling is CI only'
 if a.mode=='build':assert a.toolchain and a.archive;build(a.root,a.toolchain,a.archive)
 else:verify(a.root)
if __name__=='__main__':main()
