# EvalPythonGrader

A PythonGrader object that runs a python script on the input. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;python&#x60;. | 
**name** | **str** | The name of the grader. | 
**source** | **str** | The source code of the python script. | 
**pass_threshold** | **float** | The threshold for the score. | [optional] 
**image_tag** | **str** | The image tag to use for the python script. | [optional] 

## Example

```python
from openapi_client.models.eval_python_grader import EvalPythonGrader

# TODO update the JSON string below
json = "{}"
# create an instance of EvalPythonGrader from a JSON string
eval_python_grader_instance = EvalPythonGrader.from_json(json)
# print the JSON string representation of the object
print(EvalPythonGrader.to_json())

# convert the object into a dict
eval_python_grader_dict = eval_python_grader_instance.to_dict()
# create an instance of EvalPythonGrader from a dict
eval_python_grader_from_dict = EvalPythonGrader.from_dict(eval_python_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


