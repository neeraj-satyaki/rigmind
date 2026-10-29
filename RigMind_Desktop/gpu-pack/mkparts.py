import zipfile, pathlib, time, json, hashlib
root=pathlib.Path(r"D:\Projects\rigmind\RigMind_Desktop"); src=root/"runtime-windows"/"gpu"
base="https://github.com/neeraj-satyaki/rigmind/releases/download/v1.0.1-win64/"
parts=[("RigMind-GPU-cudnn-8.9.7-win64.zip",["cudnn64_8","cudnn_ops_infer64_8","cudnn_adv_infer64_8","cudnn_cnn_infer64_8"],"cudnn64_8.dll"),
       ("RigMind-GPU-cuda-11.8-win64.zip",["cudart64_110","cublas64_11","cublasLt64_11","cufft64_10","curand64_10"],"cudart64_110.dll")]
out=[]
for name,dlls,marker in parts:
    t0=time.time(); z=root/name; z.unlink(missing_ok=True)
    with zipfile.ZipFile(z,"w",zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as zf:
        for d in dlls: zf.write(src/(d+".dll"),"gpu/"+d+".dll")
    h=hashlib.sha256()
    with open(z,"rb") as f:
        for chunk in iter(lambda:f.read(1<<20),b""): h.update(chunk)
    size=z.stat().st_size
    print("%s  %d bytes (%.0f MB)  sha256 %s  %.0f s" % (name,size,size/1048576,h.hexdigest().upper(),time.time()-t0), flush=True)
    out.append({"file":name,"bytes":size,"sha256":h.hexdigest().upper(),"url":base+name,"marker":marker})
json.dump(out,open(root/"gpu-pack"/"parts.json","w"),indent=1)
