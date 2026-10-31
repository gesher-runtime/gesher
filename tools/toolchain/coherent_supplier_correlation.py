"""Same-attempt selected supplier byte comparison, CI artifact inputs only."""
import hashlib,json,os,sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/native3-locks'))
sys.path.insert(0,str(ROOT/'tools/net-tcp-priority'))
sys.path.insert(0,str(ROOT/'tools/host-pthread-authority'))
import coherent_tcp_context
import minimum_receipt as minimum
import prepare_host_pthread as factory
import selected_supplier
import union_payload
import host_roles

def compare(full,tcp):
    # Each authority has already passed its own complete role validator.
    for key in ('role','pin','source_files','compiler_archive_sha256','files','public_headers',
                'private_record_layout','original_profile','compiler_driver_sha256','cxx_driver_sha256'):
        assert full[key]==tcp[key],('Selected supplier mismatch',key)
    assert minimum.exports(full['dll_exports'])==minimum.exports(tcp['dll_exports'])
    assert minimum.imports(full['dll_imports'])==minimum.imports(tcp['dll_imports'])
    assert {x['sha256'] for x in full['dll_symbol_objects'].values()}=={x['sha256'] for x in tcp['dll_symbol_objects'].values()}
    for authority in (full,tcp):
        assert authority['files']['lib/libpthread.a']==authority['files']['lib/libwinpthread.a']
        assert authority['files']['lib/libpthread.dll.a']==authority['files']['lib/libwinpthread.dll.a']
    return {'selected_library_sha256':full['files'],'headers':full['public_headers'],
            'compiler_archive_sha256':full['compiler_archive_sha256'],
            'compiler_driver_sha256':full['compiler_driver_sha256'],
            'underlying_clang_equality':'UNKNOWN_NOT_IN_FULL_RECEIPT'}

def main():
    context=coherent_tcp_context.context(os.environ,'supplier-correlation')
    info=host_roles.context()
    host=Path('ci-suppliers/native3_host');tcpdir=Path('ci-suppliers/tcp_selected')
    actual=union_payload.check_manifest(host,'native3-common-host-fixture',union_payload.HOST_FILES,info,union_payload.source_inventory())
    name=host_roles.receipt_name('native3-common-host-fixture')
    receipt=(host/name).read_bytes();full=json.loads(receipt)
    factory.check_metadata(full,context)
    selected=actual['compiler']['selected_toolchain']
    assert selected['receipt_sha256']==hashlib.sha256(receipt).hexdigest()
    manifest=json.loads((tcpdir/'provenance.json').read_text(encoding='utf-8'))
    assert (manifest['source'],str(manifest['run']),str(manifest['attempt']),manifest['repository'],str(manifest['repository_id'])) == (context['GITHUB_SHA'],context['GITHUB_RUN_ID'],context['GITHUB_RUN_ATTEMPT'],context['GITHUB_REPOSITORY'],context['GITHUB_REPOSITORY_ID'])
    assert manifest['producer_job']=='compile' and manifest['supplier_selection']=='selected-minimum'
    selected_supplier.check_selected(manifest['supplier'],context)
    result=compare(full['authority'],manifest['supplier']['minimum_authority'])
    print('COHERENT_SELECTED_SUPPLIER_CORRELATION '+json.dumps(dict(source=context['GITHUB_SHA'],run=context['GITHUB_RUN_ID'],attempt=context['GITHUB_RUN_ATTEMPT'],result=result,scope='same selected supplier bytes; underlying compiler unproved'),sort_keys=True),flush=True)

if __name__=='__main__':main()
