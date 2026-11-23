"""Authenticated same-run selected suppliers, including retained CI producers."""
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
import component_producer

def compare(full,tcp):
    # Each authority has already passed its own complete role validator.
    for key in ('role','pin','source_files','compiler_archive_sha256','public_headers',
                'private_record_layout','original_profile','compiler_driver_sha256','cxx_driver_sha256'):
        assert full[key]==tcp[key],('Selected supplier mismatch',key)
    assert minimum.exports(full['dll_exports'])==minimum.exports(tcp['dll_exports'])
    assert minimum.imports(full['dll_imports'])==minimum.imports(tcp['dll_imports'])
    for authority in (full,tcp):
        assert authority['files']['lib/libpthread.a']==authority['files']['lib/libwinpthread.a']
        assert authority['files']['lib/libpthread.dll.a']==authority['files']['lib/libwinpthread.dll.a']
    same_files = (full['files'] == tcp['files'])
    same_objects = ({x['sha256'] for x in full['dll_symbol_objects'].values()} == {x['sha256'] for x in tcp['dll_symbol_objects'].values()})
    return {'selected_library_sha256':full['files'],
            'tcp_library_sha256':tcp['files'],
            'same_binary_bytes':same_files and same_objects,
            'headers':full['public_headers'],
            'compiler_archive_sha256':full['compiler_archive_sha256'],
            'compiler_driver_sha256':full['compiler_driver_sha256'],
            'underlying_clang_equality':'UNKNOWN_NOT_IN_FULL_RECEIPT'}

def main():
    context=coherent_tcp_context.context(os.environ,'supplier-correlation')
    info=host_roles.context()
    host=Path('ci-suppliers/native3_host');tcpdir=Path('ci-suppliers/tcp_selected')
    actual=union_payload.check_manifest(host,'native3-common-host-fixture',union_payload.HOST_FILES,info,union_payload.source_inventory())
    name=host_roles.receipt_name('native3-common-host-fixture')
    if not (host/name).exists() or 'selected_toolchain' not in actual.get('compiler', {}):
        print('COHERENT_SELECTED_SUPPLIER_CORRELATION '+json.dumps(dict(source=context['GITHUB_SHA'],run=context['GITHUB_RUN_ID'],attempt=context['GITHUB_RUN_ATTEMPT'],skipped='publisher_build',scope='host build did not use selected supplier'),sort_keys=True),flush=True)
        return
    receipt=(host/name).read_bytes();full=json.loads(receipt)
    # check_manifest authenticated this host role independently.  Its selected
    # receipt records that producer's attempt, which may differ from the TCP
    # producer and from the current correlation job.
    host_context = component_producer.supplier_context(actual['context'])
    factory.check_metadata(full,host_context)
    selected=actual['compiler']['selected_toolchain']
    assert selected['receipt_sha256']==hashlib.sha256(receipt).hexdigest()
    manifest, tcp_producer = component_producer.verify('tcp-selected-correlation',
                                                      path=tcpdir/'provenance.json', consumer=info)
    tcp_context = component_producer.supplier_context(tcp_producer)
    assert (manifest['source'],str(manifest['run']),str(manifest['attempt']),manifest['repository'],str(manifest['repository_id'])) == (tcp_context['GITHUB_SHA'],tcp_context['GITHUB_RUN_ID'],tcp_context['GITHUB_RUN_ATTEMPT'],tcp_context['GITHUB_REPOSITORY'],tcp_context['GITHUB_REPOSITORY_ID'])
    assert manifest['producer_job']=='compile' and manifest['supplier_selection']=='selected-minimum'
    selected_supplier.check_selected(manifest['supplier'],tcp_context)
    result=compare(full['authority'],manifest['supplier']['minimum_authority'])
    print('COHERENT_SELECTED_SUPPLIER_CORRELATION '+json.dumps(dict(source=context['GITHUB_SHA'],run=context['GITHUB_RUN_ID'],attempt=context['GITHUB_RUN_ATTEMPT'],host_producer_attempt=host_context['GITHUB_RUN_ATTEMPT'],tcp_producer_attempt=tcp_context['GITHUB_RUN_ATTEMPT'],result=result,scope='same selected supplier bytes; underlying compiler unproved'),sort_keys=True),flush=True)

def diagnostic_main():
    """Verify a real publisher host alongside separate fork TCP diagnostics."""
    import fork_supplier as fork
    descriptor = fork.identity.current('supplier-correlation')
    info = host_roles.context()
    host = Path('ci-suppliers/native3_host')
    tcpdir = Path('ci-suppliers/tcp_selected')
    sources = union_payload.source_inventory()
    actual = union_payload.check_manifest(host, 'native3-common-host-fixture',
                                         union_payload.HOST_FILES, info, sources)
    # Feature-branch full builds use the existing publisher cancellation repair.
    # A selected full-host role needs a separate authority design, not a flag.
    assert 'selected_toolchain' not in actual['compiler']
    assert not (host / host_roles.receipt_name('native3-common-host-fixture')).exists()
    import prepare_pthread_cancel as publisher
    production = actual['compiler']['production_pthread']
    assert production['role'] == publisher.PIN['role']
    assert production['recipe'] == publisher.recipe()
    assert production['patch_sha256'] == publisher.PIN['patch_sha256']
    assert production['thread_source_sha256'] == publisher.PIN['thread_output_sha256']
    assert production['context']['commit'] == descriptor['context']['GITHUB_SHA']
    assert production['context']['tracked_changes'] is False
    assert production['context']['actions']['GITHUB_JOB'] == 'build'
    assert all(production['context']['actions'][k] == v for k, v in descriptor['context'].items())
    manifest = fork.check_artifact(tcpdir, descriptor, sources)
    print('FORK_TCP_DIAGNOSTIC_CORRELATION ' + json.dumps(dict(
        evidence_schema=fork.SCHEMA, diagnostic=descriptor,
        host_scope='publisher-cancel', tcp_scope='selected-minimum-fork-diagnostic',
        host_compiler=actual['compiler'],
        tcp_minimum_receipt_sha256=manifest['supplier']['minimum_authority_sha256'],
        selected_full_host_comparison='not-applicable-publisher-host'), sort_keys=True), flush=True)


if __name__=='__main__':
    if sys.argv[1:] == ['--fork-diagnostic']:
        diagnostic_main()
    else:
        assert not sys.argv[1:], 'Unknown supplier correlation invocation'
        main()
