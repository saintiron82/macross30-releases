"""Keep work/review annotations out of player-visible subtitle text."""
import re

NOTE=re.compile(r'청취|원음|검토|확인\s*필요|미확인|인식\s*(?:불가|오류)|ASR|inaudible|uncertain|review',re.I)
BRACKETS=re.compile(r'\[[^\]\r\n]*\]|\([^()\r\n]*\)|【[^】\r\n]*】')
# These existing annotations wrap an actual tentative translation, rather than
# a descriptor such as '이름' or '지명'. Remove the label, retain the utterance.
UTTERANCES={
    '록온 경보?','긴급 지원을 요청한다!','군용 고성능 ECM인가?','대응 전투',
    '정비를 게을리하지 마라.','바사라?','미나?','그런 역할이었기 때문에','전선에 나가는 사람',
}
GAP='\ue000'


def clean_caption_text(text):
    def replace(match):
        value=match.group()[1:-1]
        if not NOTE.search(value):return match.group()
        rest=re.split(r'[:：]',value,maxsplit=1)
        if len(rest)==2 and rest[1].strip() in UTTERANCES:return rest[1].strip()
        return GAP
    text=BRACKETS.sub(replace,text)
    # A placeholder's particle belongs to the missing placeholder, not the
    # surrounding sentence. Do not leave '적기가 에 접근' on screen.
    text=re.sub(GAP+r'(?:에게|에서|으로|은|는|이|가|을|를|의|에|로)(?=\s|[,?.!]|$)','',text)
    text=text.replace(GAP,'')
    text=text.replace('브레라 씨와 지켜 줘서','브레라 씨가 지켜 줘서')
    text=re.sub(r'[ \t]+',' ',text)
    text=re.sub(r'[ \t]+([,!.?])',r'\1',text)
    text=re.sub(r',\s*([.!?])',r'\1',text)
    text=re.sub(r'^\s*[,;:]\s*','',text)
    text='\n'.join(line.strip() for line in text.splitlines() if line.strip()).strip()
    return text


def has_work_annotation(text):
    return any(NOTE.search(m.group()) for m in BRACKETS.finditer(text))
