import zipfile, pathlib, time
t0=time.time(); root=pathlib.Path(r'D:\Projects\rigmind\RigMind_Desktop'); out=pathlib.Path(r'D:\Projects\rigmind\RigMind_Desktop\RigMind-GPU-Runtime-1.0.0-win64.zip')
out.unlink(missing_ok=True)
with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as z:
    for f in sorted((root/'runtime-windows'/'gpu').glob('*.dll')): z.write(f, 'gpu/'+f.name)
print('zip: %d bytes (%.2f GiB) in %.0f s' % (out.stat().st_size, out.stat().st_size/2**30, time.time()-t0))
