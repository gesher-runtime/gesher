"""Pure Loaded/nm metadata: actual411477 text, explicitly synthetic nm sets."""
import contextlib,importlib.util,io
from pathlib import Path
spec=importlib.util.spec_from_file_location('owner_host_manifest',Path(__file__).with_name('owner-host-manifest.py'));proof=importlib.util.module_from_spec(spec);spec.loader.exec_module(proof)
# Actual full raw LLD rows from b120e5050117807318793e1a3763153cf76885ba983ed38396e587b69000faf8.
READ='gesher::ReadFontEnvironment(gesher::FontEnvironmentKey)'
DEVICE='gesher::RegisterDevice(int, gesher::Device)'
ACTUAL=('ld.lld: Loaded libgesher_posix.a(font_environment.cc.obj) for '+READ+'\n'
        'ld.lld: Loaded libgesher_posix.a(fd.cc.obj) for '+DEVICE+'\n'
        'ld.lld: Loaded libgesher_posix.a(unistd.cc.obj) for setenv\n')
# No raw/demangled nm output was emitted before the original failure. These
# memberships are SYNTHETIC controls, not reconstructed binary evidence.
raw={'synthetic_raw_read'};demangled={READ}
with contextlib.redirect_stdout(io.StringIO()):
 rows=proof.loaded_rows(ACTUAL)
 assert proof.bind_loaded(rows,'font_environment.cc.obj',raw,demangled)[0]['required']==READ
 assert proof.bind_loaded(rows,'fd.cc.obj',{'synthetic_device'}, {DEVICE})[0]['required']==DEVICE
 assert proof.bind_loaded(rows,'unistd.cc.obj',{'setenv'}, {'setenv'})[0]['required']=='setenv'
 assert proof.bind_loaded(proof.loaded_rows(ACTUAL+ACTUAL),'fd.cc.obj',{'synthetic_device'}, {DEVICE})[0]['required']==DEVICE
 assert proof.bind_loaded(proof.loaded_rows('ld.lld: Loaded libgesher_posix.a(font_environment.cc.obj) for synthetic_raw_read'),'font_environment.cc.obj',raw,set())[0]['required_format']=='raw'
 negatives=0
 cases=[('truncated-symbol',ACTUAL.replace(READ,'gesher::ReadFontEnvironment('),'font_environment.cc.obj',raw,demangled),
        ('wrong-member',ACTUAL.replace('font_environment.cc.obj','other.cc.obj'),'font_environment.cc.obj',raw,demangled),
        ('unknown-symbol',ACTUAL.replace(READ,'unknown_symbol'),'font_environment.cc.obj',raw,demangled),
        ('foreign-archive',ACTUAL.replace('libgesher_posix.a','libforeign.a'),'font_environment.cc.obj',raw,demangled),
        ('foreign-member-global-definitions',ACTUAL,'font_environment.cc.obj',{'defined_only_by_another_member'},{DEVICE}),
        ('trailing-forgery',ACTUAL.replace(READ,READ+' trailing-forgery'),'font_environment.cc.obj',raw,demangled),
        ('malformed-Loaded',ACTUAL+'ld.lld: Loaded libgesher_posix.a(font_environment.cc.obj) for\n','font_environment.cc.obj',raw,demangled),
        ('ambiguous-two-defined',ACTUAL+'ld.lld: Loaded libgesher_posix.a(font_environment.cc.obj) for another_defined\n','font_environment.cc.obj',raw|{'another_defined'},demangled)]
 for label,text,member,actual_raw,actual_demangled in cases:
  try:proof.bind_loaded(proof.loaded_rows(text),member,actual_raw,actual_demangled)
  except AssertionError:negatives+=1
  else:raise AssertionError('Accepted Loaded/member control '+label)
print('FONT_OWNER_HOST_LOADED_METADATA_PASS actual_raw_golden=3 full_demangled_spaces=RETAINED identical_duplicates=COALESCED per_member_nm_sets=SYNTHETIC negatives='+str(negatives)+' native_processes_started=0')
