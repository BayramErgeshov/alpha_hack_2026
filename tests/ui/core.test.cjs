const test = require('node:test');
const assert = require('node:assert/strict');
const {calculate,safeUrl,pageItems,isCurrency} = require('../../ui/core.js');
test('currency scenario returns correct increase and proportional bars',()=>{
  const result=calculate(1000000,80,88);
  assert.equal(result.before,80000000);assert.equal(result.after,88000000);assert.equal(result.delta,8000000);
  assert.ok(Math.abs(result.percent-10)<1e-10);assert.ok(Math.abs(result.baseHeight/result.rateHeight-80/88)<1e-10);
});
test('declining and unchanged rates retain sign and scale',()=>{
  assert.equal(calculate(100,100,50).percent,-50);assert.equal(calculate(100,100,50).delta,-5000);
  assert.equal(calculate(100,100,100).delta,0);
  assert.equal(calculate(1,.01,.01).before,.01);
});
test('invalid financial values do not produce a result',()=>{
  for(const args of [[0,80,88],[1,0,88],[1,80,0],[-1,80,88],[NaN,80,88],[1,Infinity,88],[1e13,80,88],[1,.001,1],[1,1,100001]]) assert.equal(calculate(...args),null);
});
test('source links reject executable or local protocols',()=>{
  for(const url of ['javascript:alert(1)','data:text/html,test','file:///etc/passwd','//evil.test','not a url']) assert.equal(safeUrl(url),null);
  assert.equal(safeUrl('https://example.com/news'),'https://example.com/news');
});
test('all feed items remain reachable with an incomplete last page',()=>{
  const items=Array.from({length:8},(_,i)=>i);
  assert.deepEqual([0,1,2].flatMap(p=>pageItems(items,p)),items);assert.deepEqual(pageItems(items,3),[]);
});
test('scenario shortcut relies on API category, never guessed from title',()=>{
  assert.equal(isCurrency({categories:['currency']}),true);assert.equal(isCurrency({title:'Доллар'}),false);assert.equal(isCurrency(null),false);
});
