"""Shared helpers for local media and caption checks."""
import json
import socket
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}):\n{result.stderr}')
    return result.stdout


def probe(path):
    return json.loads(run('ffprobe','-v','error','-show_streams','-show_packets','-show_frames',
                          '-show_entries','stream=index,codec_type,codec_name,profile,width,height,level,field_order,r_frame_rate,sample_rate,channels,color_space,color_primaries,color_transfer:'
                          'packet=stream_index,pts_time,dts_time,size:'
                          'frame=media_type,key_frame,pict_type,best_effort_timestamp_time,side_data_list',
                          '-of','json',path))


def validate(path, duration, bitrate=14000000):
    info=probe(path)
    video=next(s for s in info['streams'] if s['codec_type']=='video')
    audio=next(s for s in info['streams'] if s['codec_type']=='audio')
    assert (video['codec_name'],video['width'],video['height'],video['field_order'],video['r_frame_rate'])==('h264',1920,1080,'progressive','30/1'),video
    assert video['profile']=='High' and video['level']==41,video
    assert all(video[key]=='bt709' for key in ['color_space','color_primaries','color_transfer']),video
    assert (audio['codec_name'],audio['profile'],audio['sample_rate'],audio['channels'])==('aac','LC','44100',2),audio
    # FFprobe combines packets and frames into packets_and_frames with both flags.
    entries=info.get('packets_and_frames',[])
    packets=[p for p in entries if p.get('type')=='packet'] or info.get('packets',[])
    frames=[p for p in entries if p.get('type')=='frame'] or info.get('frames',[])
    dts=[float(p['dts_time']) for p in packets]
    assert all(b>=a for a,b in zip(dts,dts[1:])), 'Global FLV DTS went backwards'
    times={}
    for stream in [video,audio]:
        rows=[p for p in packets if p['stream_index']==stream['index']]
        values=sorted(float(p['pts_time']) for p in rows)
        times[stream['codec_type']]=(values[0],values[-1])
        assert all(b>a for a,b in zip(values,values[1:])),stream
    v,a=times['video'],times['audio']
    assert 0<=min(v[0],a[0]) and max(v[0],a[0])<0.2,times
    assert abs(v[0]-a[0])<0.08 and abs((v[1]-a[1])-(v[0]-a[0]))<0.15,times
    assert v[1]-v[0]>duration-0.15,times
    vf=[f for f in frames if f.get('media_type')=='video']
    presentation=[float(f['best_effort_timestamp_time']) for f in vf]
    assert all(abs((b-a)-1/30)<0.0011 for a,b in zip(presentation,presentation[1:])), 'Output is not constant 30 fps'
    keys=[float(f['best_effort_timestamp_time']) for f in vf if f['key_frame']]
    if duration>=4:
        assert len(keys)>=2,keys
        assert all(abs((b-a)-2)<0.002 for a,b in zip(keys,keys[1:])),keys
    assert any(f.get('pict_type')=='B' for f in vf), 'B-frame profile was lost'
    assert any('ATSC A53' in s.get('side_data_type','') for f in vf for s in f.get('side_data_list',[])), 'Embedded CC missing'
    # Measure steady video packet bytes, after startup CPB fill settles.
    sizes=[int(p['size']) for p in packets if p['stream_index']==video['index'] and 2<=float(p['dts_time'])<duration-0.1]
    if duration>=6:
        rate=sum(sizes)*8/(duration-0.1-2)
        assert 0.85*bitrate<rate<1.15*bitrate,rate
    print(f'  PASS profile/cadence/GOP/AAC/captions/A-V timing: {path.name}',flush=True)


def caption_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]
